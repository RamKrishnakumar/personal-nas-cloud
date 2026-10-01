#include <QTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "handlers/FileRouter.h"
#include "http/Compression.h"
#include "http/Connection.h"
#include "http/HttpServer.h"
#include "http/RequestParser.h"
#include "http/Router.h"

namespace {

/** One parsed HTTP response. */
struct Response
{
    int        status = 0;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;

    QByteArray header(const QByteArray& lowercaseName) const
    {
        for (const auto& [name, value] : headers) {
            if (name == lowercaseName)
                return value;
        }
        return {};
    }
};

/**
 * @brief Spins the event loop until @p predicate holds, or the timeout expires.
 *
 * Pumping the event loop is mandatory here, and is easy to get wrong. The server
 * runs on this same thread, so QTcpServer only calls incomingConnection() when
 * this thread processes events. QAbstractSocket::waitForReadyRead() does not
 * qualify: it polls one socket's engine without running the event loop, so the
 * server never accepts and the request is never answered — while the client still
 * appears connected, because the OS completed the handshake into the listen
 * backlog on its own.
 */
template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();

    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

/**
 * @brief Reads responses from one socket, one at a time.
 *
 * Holds its own buffer across calls, which is what makes the pipelining test
 * meaningful: the second response often arrives in the same TCP segment as the
 * first, so a reader that discarded leftover bytes would then wait forever for
 * data it had already thrown away — and would report the server as broken.
 */
class ResponseReader
{
public:
    explicit ResponseReader(QTcpSocket* socket) : m_socket(socket) {}

    /** Reads one whole response, consuming exactly its bytes from the buffer. */
    bool read(Response* out, int timeoutMs = 5000)
    {
        const auto drain = [this] {
            if (m_socket->bytesAvailable() > 0)
                m_buffer += m_socket->readAll();
        };

        if (!spinUntil([&] { drain(); return m_buffer.contains("\r\n\r\n"); }, timeoutMs))
            return false;

        const qsizetype headerEnd = m_buffer.indexOf("\r\n\r\n");
        const QList<QByteArray> lines = m_buffer.left(headerEnd).split('\n');
        if (lines.isEmpty())
            return false;

        const QList<QByteArray> statusParts = lines.first().simplified().split(' ');
        if (statusParts.size() < 2)
            return false;
        out->status = statusParts.at(1).toInt();

        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line  = lines.at(i).trimmed();
            const qsizetype  colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            out->headers.append({ line.left(colon).toLower(),
                                  line.mid(colon + 1).trimmed() });
        }

        const QByteArray lengthValue = out->header("content-length");
        const qint64 expected = lengthValue.isEmpty() ? 0 : lengthValue.toLongLong();
        const qsizetype bodyStart = headerEnd + 4;

        if (!spinUntil([&] { drain(); return m_buffer.size() - bodyStart >= expected; },
                       timeoutMs)) {
            return false;
        }

        out->body = m_buffer.mid(bodyStart, expected);

        // Consume exactly this response, leaving any pipelined remainder in place.
        m_buffer.remove(0, bodyStart + expected);
        return true;
    }

private:
    QTcpSocket* m_socket;
    QByteArray  m_buffer;
};

/** Records which thread handled each request, then delegates. */
class ThreadRecordingRouter : public http::Router
{
public:
    explicit ThreadRecordingRouter(http::Router* inner) : m_inner(inner) {}

    void handle(http::Connection& connection, const http::Request& request) override
    {
        {
            QMutexLocker locker(&m_mutex);
            m_threads.insert(QThread::currentThread());
        }
        m_inner->handle(connection, request);
    }

    QSet<QThread*> threads() const
    {
        QMutexLocker locker(&m_mutex);
        return m_threads;
    }

private:
    http::Router*          m_inner;
    mutable QMutex         m_mutex;
    QSet<QThread*>         m_threads;
};

} // namespace

/**
 * Integration tests for the v0.6 HTTP stack.
 *
 * Unit tests cover the parsers; these drive real sockets, because step 5 changes
 * runtime behaviour: which thread serves a request, whether a connection is
 * reused, and whether declared byte counts match what is actually sent.
 */
class TestHttpServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void servesDirectoryListing();
    void escapesFilenamesInListing();
    void servesWholeFileWithExactLength();
    void videoProbeRangeIsHonoured();
    void explicitRangeReturnsExactBytes();
    void openEndedRangeReturnsRemainder();
    void suffixRangeReturnsTail();
    void unsatisfiableRangeReturns416();
    void conditionalRequestReturns304();
    void headSendsHeadersWithoutBody();
    void missingPathReturns404();
    void traversalReturns403();
    void keepAliveReusesOneConnection();
    void pipelinedRequestsAreBothAnswered();
    void malformedRequestGetsBadRequest();
    void concurrentClientsAreAllServed();
    void requestsAreHandledOffTheMainThread();
    void listingIsGzippedWhenTheClientAcceptsIt();
    void listingIsNotCompressedWithoutAcceptEncoding();
    void fileBodiesAreNeverCompressed();
    void aRealHttpClientDecodesOurGzip();

private:
    QTcpSocket* connectToServer();
    Response    request(const QByteArray& raw);

    QTemporaryDir m_tmp;
    QString       m_root;
    QByteArray    m_fileContents;

    std::unique_ptr<handlers::FileRouter>      m_fileRouter;
    std::unique_ptr<ThreadRecordingRouter>     m_router;
    std::unique_ptr<http::HttpServer>          m_server;
    quint16                                    m_port = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestHttpServer::initTestCase()
{
    QVERIFY2(m_tmp.isValid(), qPrintable(m_tmp.errorString()));
    m_root = m_tmp.path();

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Album")));

    // Deterministic, non-repeating contents so a wrong offset is visible.
    m_fileContents.resize(10'000);
    for (int i = 0; i < m_fileContents.size(); ++i)
        m_fileContents[i] = static_cast<char>('A' + (i % 26));

    QFile media(m_root + QStringLiteral("/Album/clip.mp4"));
    QVERIFY(media.open(QIODevice::WriteOnly));
    QCOMPARE(media.write(m_fileContents), m_fileContents.size());
    media.close();

    // A name needing HTML escaping. Ampersand and apostrophe are chosen because
    // they are legal on NTFS; Windows forbids " < > in filenames, so the harsher
    // characters can only be exercised on POSIX.
    QFile awkward(m_root + QStringLiteral("/Album/A&B 'x'.jpg"));
    QVERIFY2(awkward.open(QIODevice::WriteOnly), qPrintable(awkward.errorString()));
    awkward.write("jpeg");
    awkward.close();

#ifndef Q_OS_WIN
    QFile harsher(m_root + QStringLiteral("/Album/qu\"ote <b>.jpg"));
    QVERIFY2(harsher.open(QIODevice::WriteOnly), qPrintable(harsher.errorString()));
    harsher.write("jpeg");
    harsher.close();
#endif

    m_fileRouter = std::make_unique<handlers::FileRouter>(m_root);
    m_router     = std::make_unique<ThreadRecordingRouter>(m_fileRouter.get());
    m_server     = std::make_unique<http::HttpServer>(m_router.get());

    QVERIFY2(m_server->start(0, QHostAddress::LocalHost),
             "Server failed to listen on an ephemeral port");
    m_port = m_server->serverPort();
    QVERIFY(m_port != 0);
}

void TestHttpServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_router.reset();
    m_fileRouter.reset();
}

QTcpSocket* TestHttpServer::connectToServer()
{
    auto* socket = new QTcpSocket(this);
    socket->connectToHost(QHostAddress::LocalHost, m_port);

    // Pumping events rather than waitForConnected(): the server shares this
    // thread, so it can only accept while this thread processes events.
    if (!spinUntil([socket] {
            return socket->state() == QAbstractSocket::ConnectedState;
        })) {
        delete socket;
        return nullptr;
    }
    return socket;
}

Response TestHttpServer::request(const QByteArray& raw)
{
    Response response;
    QTcpSocket* socket = connectToServer();
    if (!socket)
        return response;

    socket->write(raw);
    socket->flush();

    ResponseReader reader(socket);
    reader.read(&response);

    socket->disconnectFromHost();
    socket->deleteLater();
    return response;
}

// ─────────────────────────────────────────────────────────────────────────────

void TestHttpServer::servesDirectoryListing()
{
    // Since step 11 a directory returns the application shell, not a rendered
    // listing. The contents arrive separately from /api/list, which tst_listapi
    // covers; here the only question is that the shell is served and framed.
    const Response r = request("GET /Album HTTP/1.1\r\nHost: h\r\n\r\n");

    QCOMPARE(r.status, 200);
    QVERIFY(r.header("content-type").startsWith("text/html"));
    QVERIFY(r.body.contains("/_static/app.js"));
    QCOMPARE(r.body.size(), r.header("content-length").toLongLong());
}

void TestHttpServer::escapesFilenamesInListing()
{
    // Filenames no longer appear in any server-generated markup, which is what
    // removed this class of bug rather than escaping it away. The shell is static,
    // so it must not mention directory contents at all; names reach the client as
    // JSON data and are inserted with textContent.
    const Response r = request("GET /Album HTTP/1.1\r\nHost: h\r\n\r\n");
    QCOMPARE(r.status, 200);

    QVERIFY2(!r.body.contains("A&B"),  "The shell must not embed filenames");
    QVERIFY2(!r.body.contains("clip.mp4"), "The shell must not embed filenames");
}

void TestHttpServer::servesWholeFileWithExactLength()
{
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");

    QCOMPARE(r.status, 200);
    QCOMPARE(r.header("content-type"), QByteArray("video/mp4"));
    QCOMPARE(r.header("accept-ranges"), QByteArray("bytes"));
    QCOMPARE(r.header("content-length").toLongLong(), m_fileContents.size());
    QCOMPARE(r.body, m_fileContents);
}

void TestHttpServer::videoProbeRangeIsHonoured()
{
    // The v0.5 regression: rangeStart of 0 failed a `> 0` test, so this probe
    // returned the entire file. Every video seek paid for a full download.
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                               "Range: bytes=0-1\r\n\r\n");

    QCOMPARE(r.status, 206);
    QCOMPARE(r.header("content-length").toLongLong(), 2);
    QCOMPARE(r.header("content-range"),
             QByteArray("bytes 0-1/") + QByteArray::number(m_fileContents.size()));
    QCOMPARE(r.body, m_fileContents.left(2));
}

void TestHttpServer::explicitRangeReturnsExactBytes()
{
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                               "Range: bytes=100-199\r\n\r\n");

    QCOMPARE(r.status, 206);
    QCOMPARE(r.header("content-length").toLongLong(), 100);
    QCOMPARE(r.body.size(), 100);
    QCOMPARE(r.body, m_fileContents.mid(100, 100));
}

void TestHttpServer::openEndedRangeReturnsRemainder()
{
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                               "Range: bytes=9000-\r\n\r\n");

    QCOMPARE(r.status, 206);
    QCOMPARE(r.header("content-length").toLongLong(), 1000);
    QCOMPARE(r.body, m_fileContents.mid(9000));
}

void TestHttpServer::suffixRangeReturnsTail()
{
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                               "Range: bytes=-500\r\n\r\n");

    QCOMPARE(r.status, 206);
    QCOMPARE(r.header("content-length").toLongLong(), 500);
    QCOMPARE(r.body, m_fileContents.right(500));
}

void TestHttpServer::unsatisfiableRangeReturns416()
{
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                               "Range: bytes=99999-\r\n\r\n");

    QCOMPARE(r.status, 416);
    QCOMPARE(r.header("content-range"),
             QByteArray("bytes */") + QByteArray::number(m_fileContents.size()));
}

void TestHttpServer::conditionalRequestReturns304()
{
    const Response first = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");
    const QByteArray etag = first.header("etag");
    QVERIFY(!etag.isEmpty());

    const Response second = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                                    "If-None-Match: " + etag + "\r\n\r\n");

    QCOMPARE(second.status, 304);
    QVERIFY2(second.body.isEmpty(), "304 must not carry a body");
}

void TestHttpServer::headSendsHeadersWithoutBody()
{
    const Response r = request("HEAD /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");

    QCOMPARE(r.status, 200);
    // Content-Length still describes what a GET would return, but no body follows.
    QCOMPARE(r.header("content-length").toLongLong(), m_fileContents.size());
    QVERIFY2(r.body.isEmpty(), "HEAD must not send a body");
}

void TestHttpServer::missingPathReturns404()
{
    const Response r = request("GET /Album/nope.jpg HTTP/1.1\r\nHost: h\r\n\r\n");
    QCOMPARE(r.status, 404);
}

void TestHttpServer::traversalReturns403()
{
    const Response r = request("GET /../outside.txt HTTP/1.1\r\nHost: h\r\n\r\n");
    QCOMPARE(r.status, 403);
}

void TestHttpServer::keepAliveReusesOneConnection()
{
    // The single largest latency win in v0.6: without this, each of the 60
    // thumbnails on a folder page costs its own TCP and TLS handshake.
    QTcpSocket* socket = connectToServer();
    QVERIFY(socket);
    ResponseReader reader(socket);

    for (int i = 0; i < 5; ++i) {
        socket->write("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                      "Range: bytes=0-9\r\n\r\n");
        socket->flush();

        Response r;
        QVERIFY2(reader.read(&r),
                 qPrintable(QStringLiteral("request %1 produced no response").arg(i)));
        QCOMPARE(r.status, 206);
        QCOMPARE(r.body, m_fileContents.left(10));
        QCOMPARE(r.header("connection"), QByteArray("keep-alive"));

        QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
    }

    QCOMPARE(m_server->connectionCount(), 1);

    socket->disconnectFromHost();
    socket->deleteLater();
}

void TestHttpServer::pipelinedRequestsAreBothAnswered()
{
    // Two requests arriving in one read. If `consumed` were wrong, the second
    // would be silently dropped.
    QTcpSocket* socket = connectToServer();
    QVERIFY(socket);

    ResponseReader reader(socket);
    socket->write("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\nRange: bytes=0-3\r\n\r\n"
                  "GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\nRange: bytes=4-7\r\n\r\n");
    socket->flush();

    Response first;
    QVERIFY(reader.read(&first));
    QCOMPARE(first.status, 206);
    QCOMPARE(first.body, m_fileContents.mid(0, 4));

    Response second;
    QVERIFY2(reader.read(&second), "Second pipelined request was dropped");
    QCOMPARE(second.status, 206);
    QCOMPARE(second.body, m_fileContents.mid(4, 4));

    socket->disconnectFromHost();
    socket->deleteLater();
}

void TestHttpServer::malformedRequestGetsBadRequest()
{
    // Absolute-form target: refused so the server cannot be used as a proxy.
    const Response proxyStyle =
        request("GET http://elsewhere/x HTTP/1.1\r\nHost: h\r\n\r\n");
    QCOMPARE(proxyStyle.status, 400);

    const Response noHost = request("GET / HTTP/1.1\r\n\r\n");
    QCOMPARE(noHost.status, 400);
}

void TestHttpServer::concurrentClientsAreAllServed()
{
    // More clients than worker threads, so requests genuinely queue and overlap.
    constexpr int ClientCount = 12;

    QList<QTcpSocket*> sockets;
    for (int i = 0; i < ClientCount; ++i) {
        QTcpSocket* socket = connectToServer();
        QVERIFY(socket);
        socket->write("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        socket->flush();
        sockets.append(socket);
    }

    int served = 0;
    for (QTcpSocket* socket : sockets) {
        Response r;
        ResponseReader reader(socket);
        if (reader.read(&r, 15'000) && r.status == 200 && r.body == m_fileContents)
            ++served;
        socket->disconnectFromHost();
        socket->deleteLater();
    }

    QCOMPARE(served, ClientCount);
}

void TestHttpServer::requestsAreHandledOffTheMainThread()
{
    // The point of step 5. In v0.5 a large download ran on the GUI thread and
    // froze the window; this asserts handling happens elsewhere.
    QVERIFY(m_server->workerCount() >= 2);

    const QSet<QThread*> used = m_router->threads();
    QVERIFY2(!used.isEmpty(), "No requests were recorded; earlier tests must run first");
    QVERIFY2(!used.contains(QThread::currentThread()),
             "A request was handled on the test's own thread");
}

void TestHttpServer::listingIsGzippedWhenTheClientAcceptsIt()
{
    // Fetch the same listing twice, compressed and not, then check the gzip
    // trailer against the uncompressed bytes. That proves the stream genuinely
    // describes this response — a wrong CRC or length produces something a client
    // discards after transferring it, and no status code reveals that.
    const Response plain = request("GET /Album HTTP/1.1\r\nHost: h\r\n"
                                   "Connection: close\r\n\r\n");
    const Response gzipped = request("GET /Album HTTP/1.1\r\nHost: h\r\n"
                                     "Accept-Encoding: gzip, deflate, br\r\n"
                                     "Connection: close\r\n\r\n");

    QCOMPARE(plain.status, 200);
    QCOMPARE(gzipped.status, 200);

    // Small test listings can fall under MinimumCompressibleBytes; only assert the
    // framing when compression actually applied.
    if (gzipped.header("content-encoding") != QByteArray("gzip")) {
        QVERIFY2(plain.body.size() < http::MinimumCompressibleBytes,
                 "A listing above the threshold was not compressed");
        return;
    }

    QCOMPARE(gzipped.header("vary"), QByteArray("Accept-Encoding"));
    QCOMPARE(quint8(gzipped.body[0]), 0x1Fu);
    QCOMPARE(quint8(gzipped.body[1]), 0x8Bu);

    const auto trailer32 = [&](qsizetype fromEnd) {
        const qsizetype at = gzipped.body.size() - fromEnd;
        return quint32(quint8(gzipped.body[at]))
             | (quint32(quint8(gzipped.body[at + 1])) << 8)
             | (quint32(quint8(gzipped.body[at + 2])) << 16)
             | (quint32(quint8(gzipped.body[at + 3])) << 24);
    };

    QCOMPARE(trailer32(4), quint32(plain.body.size()));
    QCOMPARE(trailer32(8), http::gzipCrc32(plain.body));

    QVERIFY2(gzipped.body.size() < plain.body.size(),
             "The compressed listing was not smaller");
}

void TestHttpServer::listingIsNotCompressedWithoutAcceptEncoding()
{
    const Response r = request("GET /Album HTTP/1.1\r\nHost: h\r\n"
                               "Connection: close\r\n\r\n");

    QCOMPARE(r.status, 200);
    QVERIFY2(r.header("content-encoding").isEmpty(),
             "A client that did not ask for gzip must not receive it");

    // Vary is still declared, so a shared cache cannot serve a gzipped copy of
    // this response to such a client.
    QCOMPARE(r.header("vary"), QByteArray("Accept-Encoding"));
}

void TestHttpServer::fileBodiesAreNeverCompressed()
{
    // Streamed files must stay uncompressed: compression would break the exact
    // Content-Length that keep-alive depends on, and byte ranges over a compressed
    // stream are meaningless. An mp4 is already compressed anyway.
    const Response r = request("GET /Album/clip.mp4 HTTP/1.1\r\nHost: h\r\n"
                               "Accept-Encoding: gzip\r\n"
                               "Connection: close\r\n\r\n");

    QCOMPARE(r.status, 200);
    QVERIFY(r.header("content-encoding").isEmpty());
    QCOMPARE(r.body, m_fileContents);
}

void TestHttpServer::aRealHttpClientDecodesOurGzip()
{
    // The framing checks verify the bytes we intended to write. This verifies a
    // client can actually inflate them: QNetworkAccessManager advertises gzip by
    // itself and decompresses transparently, so a malformed stream shows up here
    // as an error or as a body that does not match. Hand-assembled gzip framing is
    // exactly the kind of thing that passes a self-consistent test and still fails
    // against a real decoder.
    const Response plain = request("GET /Album HTTP/1.1\r\nHost: h\r\n"
                                   "Connection: close\r\n\r\n");
    QCOMPARE(plain.status, 200);

    QNetworkAccessManager manager;
    QNetworkRequest networkRequest(
        QUrl(QStringLiteral("http://127.0.0.1:%1/Album").arg(m_port)));

    QNetworkReply* reply = manager.get(networkRequest);
    QVERIFY(spinUntil([reply] { return reply->isFinished(); }, 10'000));

    QCOMPARE(reply->error(), QNetworkReply::NoError);

    const QByteArray received = reply->readAll();
    const QByteArray encoding =
        reply->rawHeader(QByteArrayLiteral("Content-Encoding"));
    reply->deleteLater();

    QCOMPARE(encoding, QByteArray("gzip"));

    // Decompressed content must be identical to what an uncompressed request
    // returns, byte for byte.
    QCOMPARE(received, plain.body);
}

QTEST_GUILESS_MAIN(TestHttpServer)
#include "tst_httpserver.moc"
