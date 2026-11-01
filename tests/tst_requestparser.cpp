#include <QTest>

#include "http/RequestParser.h"

using http::ParseResult;
using http::ParseStatus;
using http::parseRequest;

namespace {

/** A minimal well-formed GET, so each test varies only what it is about. */
QByteArray get(const QByteArray& target = "/",
               const QByteArray& extraHeaders = QByteArray())
{
    return "GET " + target + " HTTP/1.1\r\nHost: nas.local\r\n"
           + extraHeaders + "\r\n";
}

/**
 * A request line carrying a raw NUL byte.
 *
 * Built by appending rather than as a sized literal: a NUL terminates the
 * literal, so the length has to be given explicitly, and getting it wrong reads
 * past the end of the array. Appending keeps the length correct by construction.
 */
QByteArray withNulInTarget()
{
    QByteArray raw = "GET /a";
    raw.append('\0');
    raw += "b HTTP/1.1\r\nHost: h\r\n\r\n";
    return raw;
}

} // namespace

/**
 * Tests for http::parseRequest.
 *
 * The parser replaces a single readAll() that assumed one socket read contained
 * exactly one whole request. Two behaviours matter most and are covered first:
 * a head split across reads must report Incomplete rather than misparse, and
 * `consumed` must be exact so pipelined bytes are not eaten.
 */
class TestRequestParser : public QObject
{
    Q_OBJECT

private slots:
    void minimalRequest();
    void splitAcrossReads();
    void pipelinedRequestsLeaveRemainder();
    void consumedExcludesBody();
    void parsesTargetAndQuery();
    void headerLookupIsCaseFolded();
    void preservesDuplicateHeaders();

    void keepAlive_data();
    void keepAlive();

    void malformed_data();
    void malformed();

    void oversizedHeadIsRejected();
    void oversizedRequestLineIsRejected();
    void unsupportedVersionIsRejected();
    void transferEncodingIsNotImplemented();
    void contentLengthParsing();
    void missingHostOnHttp11IsRejected();
    void http10NeedsNoHost();
};

// ─────────────────────────────────────────────────────────────────────────────

void TestRequestParser::minimalRequest()
{
    const QByteArray raw = get("/Photos");
    const ParseResult r = parseRequest(raw);

    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.consumed, raw.size());
    QCOMPARE(r.request.method, QByteArray("GET"));
    QCOMPARE(r.request.path, QByteArray("/Photos"));
    QCOMPARE(r.request.minorVersion, 1);
    QCOMPARE(r.request.contentLength, 0);
    QVERIFY(r.request.keepAlive);
}

void TestRequestParser::splitAcrossReads()
{
    // The regression this module exists for: v0.5 called readAll() once and
    // parsed whatever had arrived, so a head split by the network became a
    // malformed request rather than a wait.
    const QByteArray full = get("/Photos/2024");

    for (qsizetype prefix = 1; prefix < full.size(); ++prefix) {
        const ParseResult partial = parseRequest(QByteArrayView(full).sliced(0, prefix));
        QVERIFY2(partial.status == ParseStatus::Incomplete,
                 qPrintable(QStringLiteral("prefix of %1 bytes should be Incomplete")
                                .arg(prefix)));
    }

    QCOMPARE(parseRequest(full).status, ParseStatus::Complete);
}

void TestRequestParser::pipelinedRequestsLeaveRemainder()
{
    // Two requests in one read. consumed must cover only the first, or the
    // second is silently discarded once keep-alive is enabled.
    const QByteArray first  = get("/a");
    const QByteArray second = get("/b");
    const QByteArray both   = first + second;

    const ParseResult r = parseRequest(both);
    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.request.path, QByteArray("/a"));
    QCOMPARE(r.consumed, first.size());

    const ParseResult next = parseRequest(QByteArrayView(both).sliced(r.consumed));
    QCOMPARE(next.status, ParseStatus::Complete);
    QCOMPARE(next.request.path, QByteArray("/b"));
}

void TestRequestParser::consumedExcludesBody()
{
    const QByteArray head = "POST /upload HTTP/1.1\r\nHost: nas.local\r\n"
                            "Content-Length: 5\r\n\r\n";
    const QByteArray raw  = head + "hello";

    const ParseResult r = parseRequest(raw);
    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.request.contentLength, 5);
    QCOMPARE(r.consumed, head.size());
    QCOMPARE(raw.sliced(r.consumed), QByteArray("hello"));
}

void TestRequestParser::parsesTargetAndQuery()
{
    const ParseResult r = parseRequest(get("/Photos/IMG%200421.jpg?download=1&x=2"));
    QCOMPARE(r.status, ParseStatus::Complete);

    // The target stays percent-encoded; PathResolver needs the raw form.
    QCOMPARE(r.request.path,  QByteArray("/Photos/IMG%200421.jpg"));
    QCOMPARE(r.request.query, QByteArray("download=1&x=2"));
}

void TestRequestParser::headerLookupIsCaseFolded()
{
    const ParseResult r = parseRequest(
        "GET / HTTP/1.1\r\nHOST: nas.local\r\nIf-None-Match: \"abc\"\r\n\r\n");

    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.request.header("host"), QByteArrayView("nas.local"));
    QCOMPARE(r.request.header("if-none-match"), QByteArrayView("\"abc\""));
    QVERIFY(r.request.header("absent").isEmpty());
}

void TestRequestParser::preservesDuplicateHeaders()
{
    const ParseResult r = parseRequest(
        get("/", "X-Tag: one\r\nX-Tag: two\r\n"));

    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.request.headerCount("x-tag"), 2);
    QCOMPARE(r.request.header("x-tag"), QByteArrayView("one"));  // first wins
}

void TestRequestParser::keepAlive_data()
{
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<bool>("expected");

    QTest::newRow("1.1 default")
        << get("/") << true;
    QTest::newRow("1.1 close")
        << get("/", "Connection: close\r\n") << false;
    QTest::newRow("1.1 close mixed case")
        << get("/", "Connection: Close\r\n") << false;
    QTest::newRow("1.1 token in list")
        << get("/", "Connection: keep-alive, Upgrade\r\n") << true;
    QTest::newRow("1.1 close in list")
        << get("/", "Connection: Upgrade, close\r\n") << false;
    QTest::newRow("1.0 default")
        << QByteArray("GET / HTTP/1.0\r\n\r\n") << false;
    QTest::newRow("1.0 keep-alive")
        << QByteArray("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n") << true;
}

void TestRequestParser::keepAlive()
{
    QFETCH(QByteArray, raw);
    QFETCH(bool, expected);

    const ParseResult r = parseRequest(raw);
    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.request.keepAlive, expected);
}

void TestRequestParser::malformed_data()
{
    QTest::addColumn<QByteArray>("raw");

    QTest::newRow("no method")
        << QByteArray(" / HTTP/1.1\r\nHost: h\r\n\r\n");
    QTest::newRow("no version")
        << QByteArray("GET /\r\nHost: h\r\n\r\n");
    QTest::newRow("extra token")
        << QByteArray("GET / HTTP/1.1 junk\r\nHost: h\r\n\r\n");
    QTest::newRow("unencoded space in target")
        << QByteArray("GET /my file HTTP/1.1\r\nHost: h\r\n\r\n");
    QTest::newRow("empty target")
        << QByteArray("GET  HTTP/1.1\r\nHost: h\r\n\r\n");

    // Refusing absolute-form stops this server acting as an open proxy.
    QTest::newRow("absolute form")
        << QByteArray("GET http://elsewhere/x HTTP/1.1\r\nHost: h\r\n\r\n");
    QTest::newRow("authority form")
        << QByteArray("CONNECT nas.local:443 HTTP/1.1\r\nHost: h\r\n\r\n");

    QTest::newRow("header without colon")
        << QByteArray("GET / HTTP/1.1\r\nHost: h\r\nBroken\r\n\r\n");
    QTest::newRow("space before colon")
        << QByteArray("GET / HTTP/1.1\r\nHost : h\r\n\r\n");
    QTest::newRow("empty header name")
        << QByteArray("GET / HTTP/1.1\r\nHost: h\r\n: value\r\n\r\n");

    // Obsolete line folding is ambiguous and a known smuggling vector.
    QTest::newRow("obs-fold")
        << QByteArray("GET / HTTP/1.1\r\nHost: h\r\nX: a\r\n  continued\r\n\r\n");

    // Line-ending disagreement between hops is how smuggling starts.
    QTest::newRow("bare LF in request line")
        << QByteArray("GET /\n HTTP/1.1\r\nHost: h\r\n\r\n");
    QTest::newRow("NUL in target")
        << withNulInTarget();

    QTest::newRow("both framing headers")
        << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n");
    QTest::newRow("conflicting content-length")
        << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
                      "Content-Length: 9\r\n\r\n");
    QTest::newRow("non numeric content-length")
        << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: abc\r\n\r\n");
    QTest::newRow("negative content-length")
        << QByteArray("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n");
    QTest::newRow("overflowing content-length")
        << QByteArray("POST / HTTP/1.1\r\nHost: h\r\n"
                      "Content-Length: 99999999999999999999\r\n\r\n");
    QTest::newRow("duplicate host")
        << QByteArray("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
}

void TestRequestParser::malformed()
{
    QFETCH(QByteArray, raw);

    const ParseResult r = parseRequest(raw);
    QCOMPARE(r.status, ParseStatus::BadRequest);
}

void TestRequestParser::oversizedHeadIsRejected()
{
    // No terminating blank line, and past the cap: must not keep buffering.
    QByteArray raw = "GET / HTTP/1.1\r\nHost: h\r\n";
    raw += QByteArray("X-Pad: ") + QByteArray(http::MaxHeaderBlockBytes, 'a') + "\r\n";

    const ParseResult r = parseRequest(raw);
    QCOMPARE(r.status, ParseStatus::HeadersTooLarge);
}

void TestRequestParser::oversizedRequestLineIsRejected()
{
    const QByteArray raw = "GET /" + QByteArray(http::MaxRequestLineBytes, 'a')
                           + " HTTP/1.1\r\nHost: h\r\n\r\n";

    const ParseResult r = parseRequest(raw);
    QCOMPARE(r.status, ParseStatus::UriTooLong);
}

void TestRequestParser::unsupportedVersionIsRejected()
{
    for (const QByteArray version : { QByteArray("HTTP/0.9"),
                                      QByteArray("HTTP/2.0"),
                                      QByteArray("HTTP/1.2") }) {
        const QByteArray raw = "GET / " + version + "\r\nHost: h\r\n\r\n";
        const ParseResult r = parseRequest(raw);
        QCOMPARE(r.status, ParseStatus::VersionNotSupported);
    }
}

void TestRequestParser::transferEncodingIsNotImplemented()
{
    // v0.6 cannot decode chunked bodies. Saying so beats mis-framing one.
    const ParseResult r = parseRequest(
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n");
    QCOMPARE(r.status, ParseStatus::NotImplemented);
}

void TestRequestParser::contentLengthParsing()
{
    // Repeated identical values are legal, unlike conflicting ones.
    const ParseResult repeated = parseRequest(
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 7\r\n"
        "Content-Length: 7\r\n\r\n");
    QCOMPARE(repeated.status, ParseStatus::Complete);
    QCOMPARE(repeated.request.contentLength, 7);

    const ParseResult zero = parseRequest(
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    QCOMPARE(zero.status, ParseStatus::Complete);
    QCOMPARE(zero.request.contentLength, 0);
}

void TestRequestParser::missingHostOnHttp11IsRejected()
{
    const ParseResult r = parseRequest("GET / HTTP/1.1\r\n\r\n");
    QCOMPARE(r.status, ParseStatus::BadRequest);
}

void TestRequestParser::http10NeedsNoHost()
{
    const ParseResult r = parseRequest("GET / HTTP/1.0\r\n\r\n");
    QCOMPARE(r.status, ParseStatus::Complete);
    QCOMPARE(r.request.minorVersion, 0);
}

QTEST_APPLESS_MAIN(TestRequestParser)
#include "tst_requestparser.moc"
