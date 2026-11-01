#include <QTest>

#include "http/Cookies.h"
#include "http/RequestParser.h"

/**
 * Tests for http::cookieValue.
 *
 * Small surface, but the prefix-matching case is a real authentication bypass
 * shape: if `nd_session_old` could answer a lookup for `nd_session`, an attacker
 * who can set any cookie on the origin chooses the session the server reads.
 */
class TestCookies : public QObject
{
    Q_OBJECT

private slots:
    void readsASingleCookie();
    void readsOneOfSeveral();
    void toleratesWhitespace();
    void returnsEmptyWhenAbsent();
    void doesNotMatchOnAPrefix();
    void doesNotMatchOnASuffix();
    void isCaseSensitive();
    void keepsEqualsSignsInsideTheValue();
    void handlesAnEmptyValue();
    void handlesAnEmptyHeader();
    void ignoresAPairWithNoName();
    void readsFromARequest();
};

void TestCookies::readsASingleCookie()
{
    QCOMPARE(http::cookieValue("nd_session=abc123", "nd_session"),
             QByteArray("abc123"));
}

void TestCookies::readsOneOfSeveral()
{
    const QByteArray header = "theme=dark; nd_session=tok; lang=en";
    QCOMPARE(http::cookieValue(header, "nd_session"), QByteArray("tok"));
    QCOMPARE(http::cookieValue(header, "theme"), QByteArray("dark"));
    QCOMPARE(http::cookieValue(header, "lang"), QByteArray("en"));
}

void TestCookies::toleratesWhitespace()
{
    // Browsers send "; " between pairs; some clients add more.
    QCOMPARE(http::cookieValue("a=1;   nd_session = tok  ; b=2", "nd_session"),
             QByteArray("tok"));
}

void TestCookies::returnsEmptyWhenAbsent()
{
    QVERIFY(http::cookieValue("theme=dark", "nd_session").isEmpty());
}

void TestCookies::doesNotMatchOnAPrefix()
{
    // The bypass shape: a longer name must not satisfy a shorter lookup.
    QVERIFY(http::cookieValue("nd_session_backup=evil", "nd_session").isEmpty());
}

void TestCookies::doesNotMatchOnASuffix()
{
    QVERIFY(http::cookieValue("evil_nd_session=evil", "nd_session").isEmpty());
}

void TestCookies::isCaseSensitive()
{
    // RFC 6265 cookie names are case-sensitive, so ND_SESSION is a different cookie.
    QVERIFY(http::cookieValue("ND_SESSION=tok", "nd_session").isEmpty());
}

void TestCookies::keepsEqualsSignsInsideTheValue()
{
    // Padded base64 ends in '='. Splitting on every '=' would truncate the token.
    QCOMPARE(http::cookieValue("nd_session=YWJjZA==", "nd_session"),
             QByteArray("YWJjZA=="));
}

void TestCookies::handlesAnEmptyValue()
{
    // What a cleared cookie looks like on the way back.
    QVERIFY(http::cookieValue("nd_session=", "nd_session").isEmpty());
}

void TestCookies::handlesAnEmptyHeader()
{
    QVERIFY(http::cookieValue("", "nd_session").isEmpty());
    QVERIFY(http::cookieValue("nd_session=tok", "").isEmpty());
}

void TestCookies::ignoresAPairWithNoName()
{
    // A leading '=' has no name to match; it must not be read as the empty name.
    QVERIFY(http::cookieValue("=orphan; other=1", "").isEmpty());
    QCOMPARE(http::cookieValue("=orphan; nd_session=tok", "nd_session"),
             QByteArray("tok"));
}

void TestCookies::readsFromARequest()
{
    const QByteArray raw =
        "GET / HTTP/1.1\r\n"
        "Host: nas\r\n"
        "Cookie: theme=dark; nd_session=from-request\r\n"
        "\r\n";

    const http::ParseResult parsed = http::parseRequest(raw);
    QCOMPARE(parsed.status, http::ParseStatus::Complete);

    QCOMPARE(http::cookieValue(parsed.request, "nd_session"),
             QByteArray("from-request"));
}

QTEST_MAIN(TestCookies)
#include "tst_cookies.moc"
