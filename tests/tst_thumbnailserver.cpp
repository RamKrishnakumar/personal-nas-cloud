#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "db/Database.h"
#include "handlers/FileRouter.h"
#include "http/HttpServer.h"
#include "media/ThumbnailCache.h"

namespace {

struct Response
{
    int        status = 0;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;

    QByteArray header(const QByteArray& lowercaseName) const
    {
        for (const auto& [name, value] : headers)
            if (name == lowercaseName)
                return value;
        return {};
    }
};

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 15'000)
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

bool readResponse(QTcpSocket* socket, Response* out, int timeoutMs = 15'000)
{
    QByteArray buffer;
    const auto drain = [&] {
        if (socket->bytesAvailable() > 0)
            buffer += socket->readAll();
    };

    if (!spinUntil([&] { drain(); return buffer.contains("\r\n\r\n"); }, timeoutMs))
        return false;

    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');

    const QList<QByteArray> statusParts = lines.first().simplified().split(' ');
    if (statusParts.size() < 2)
        return false;
    out->status = statusParts.at(1).toInt();

    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0)
            out->headers.append({ line.left(colon).toLower(), line.mid(colon + 1).trimmed() });
    }

    const qint64 expected = out->header("content-length").toLongLong();
    if (!spinUntil([&] { drain(); return buffer.size() - (headerEnd + 4) >= expected; },
                   timeoutMs)) {
        return false;
    }

    out->body = buffer.mid(headerEnd + 4, expected);
    return true;
}

bool writeImage(const QString& path, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb((x * 5) % 256, (y * 13) % 256, (x + y) % 256));
    return image.save(path);
}

} // namespace

/**
 * End-to-end thumbnail serving.
 *
 * The behaviour that matters here cannot be seen from a unit test: whether a cold
 * folder is answered without a worker thread parking on the decode queue, and
 * whether the bytes a browser actually receives are a fraction of the original.
 */
class TestThumbnailServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void coldMissIsGeneratedAndServed();
    void servedThumbnailIsFarSmallerThanTheOriginal();
    void thumbnailsAreImmutablyCacheable();
    void mediumTierIsServed();
    void listingPointsTheGridAtThumbnails();
    void manyColdMissesAreAllServed();
    void unknownTierIsRejected();
    void traversalThroughThumbnailRouteIsRefused();
    void nonImageThumbnailRequestFails();

private:
    QTcpSocket* connect_();
    Response    request(const QByteArray& raw);

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<db::Database>           m_database;
    std::unique_ptr<media::ThumbnailCache>  m_cache;
    std::unique_ptr<handlers::FileRouter>   m_router;
    std::unique_ptr<http::HttpServer>       m_server;
    quint16                                 m_port = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailServer::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.path() + QStringLiteral("/nas");
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Album")));

    for (int i = 0; i < 12; ++i) {
        QVERIFY(writeImage(m_root + QStringLiteral("/Album/photo-%1.png").arg(i),
                           2000, 1500));
    }

    QFile notes(m_root + QStringLiteral("/Album/notes.txt"));
    QVERIFY(notes.open(QIODevice::WriteOnly));
    notes.write("not an image");
    notes.close();

    m_database = std::make_unique<db::Database>();
    QVERIFY(m_database->open(m_tmp.path() + QStringLiteral("/index.sqlite")));

    m_cache = std::make_unique<media::ThumbnailCache>(
        m_database.get(), m_tmp.path() + QStringLiteral("/thumbs"), 2);

    m_router = std::make_unique<handlers::FileRouter>(m_root);
    m_router->setThumbnailCache(m_cache.get());

    m_server = std::make_unique<http::HttpServer>(m_router.get());
    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();
}

void TestThumbnailServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_router.reset();
    m_cache.reset();
    m_database.reset();
}

QTcpSocket* TestThumbnailServer::connect_()
{
    auto* socket = new QTcpSocket(this);
    socket->connectToHost(QHostAddress::LocalHost, m_port);
    if (!spinUntil([socket] { return socket->state() == QAbstractSocket::ConnectedState; },
                   5000)) {
        delete socket;
        return nullptr;
    }
    return socket;
}

Response TestThumbnailServer::request(const QByteArray& raw)
{
    Response response;
    QTcpSocket* socket = connect_();
    if (!socket)
        return response;

    socket->write(raw);
    socket->flush();
    readResponse(socket, &response);
    socket->abort();
    socket->deleteLater();
    return response;
}

// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailServer::coldMissIsGeneratedAndServed()
{
    const Response r = request("GET /_thumb/s/Album/photo-0.png HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(r.status, 200);
    QVERIFY(!r.body.isEmpty());

    const QImage image = QImage::fromData(r.body);
    QVERIFY2(!image.isNull(), "The served bytes were not a decodable image");
    QCOMPARE(qMax(image.width(), image.height()),
             media::tierLongEdge(media::ThumbTier::Small));
}

void TestThumbnailServer::servedThumbnailIsFarSmallerThanTheOriginal()
{
    const Response original = request("GET /Album/photo-1.png HTTP/1.1\r\n"
                                      "Host: h\r\nConnection: close\r\n\r\n");
    const Response thumb = request("GET /_thumb/s/Album/photo-1.png HTTP/1.1\r\n"
                                   "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(original.status, 200);
    QCOMPARE(thumb.status, 200);

    // Compared on pixels, not bytes. A byte ratio depends on image content, and a
    // synthetic test pattern misrepresents it in both directions; the server only
    // controls how many pixels it sends. On real camera JPEGs the byte saving is
    // the dramatic one — that is what turns a folder page from hundreds of
    // megabytes into a few — but it is not something a test can assert portably.
    const QImage originalImage = QImage::fromData(original.body);
    const QImage thumbImage    = QImage::fromData(thumb.body);
    QVERIFY(!originalImage.isNull());
    QVERIFY(!thumbImage.isNull());

    const qint64 originalPixels = qint64(originalImage.width()) * originalImage.height();
    const qint64 thumbPixels    = qint64(thumbImage.width()) * thumbImage.height();

    QVERIFY2(thumbPixels * 20 < originalPixels,
             qPrintable(QStringLiteral("thumbnail %1 pixels vs original %2")
                            .arg(thumbPixels).arg(originalPixels)));
}

void TestThumbnailServer::thumbnailsAreImmutablyCacheable()
{
    const Response r = request("GET /_thumb/s/Album/photo-2.png?v=abc123 HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(r.status, 200);

    // Safe only because the URL carries the source file's mtime and size. Without
    // that, a year-long cache lifetime would pin a stale image.
    const QByteArray cacheControl = r.header("cache-control");
    QVERIFY2(cacheControl.contains("immutable"),
             qPrintable(QStringLiteral("Cache-Control was '%1'")
                            .arg(QString::fromUtf8(cacheControl))));
    QVERIFY(cacheControl.contains("max-age=31536000"));
}

void TestThumbnailServer::mediumTierIsServed()
{
    const Response r = request("GET /_thumb/m/Album/photo-3.png HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(r.status, 200);
    const QImage image = QImage::fromData(r.body);
    QVERIFY(!image.isNull());
    QCOMPARE(qMax(image.width(), image.height()),
             media::tierLongEdge(media::ThumbTier::Medium));
}

void TestThumbnailServer::listingPointsTheGridAtThumbnails()
{
    // The listing is JSON from step 11 onwards. Thumbnail URLs are still produced
    // server-side so the client never derives a cache key, and both tiers must be
    // offered: "t" for the grid, "p" for the preview overlay.
    const Response r = request("GET /api/list?path=/Album HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(r.status, 200);

    const QJsonArray entries = QJsonDocument::fromJson(r.body)
                                   .object()
                                   .value(QStringLiteral("entries")).toArray();
    QVERIFY(!entries.isEmpty());

    int withThumbnails = 0;
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("k")).toString() != QLatin1String("image"))
            continue;

        const QString grid    = entry.value(QStringLiteral("t")).toString();
        const QString preview = entry.value(QStringLiteral("p")).toString();

        QVERIFY2(grid.startsWith(QLatin1String("/_thumb/s/")),
                 "The grid is still pointed at the original rather than a thumbnail");
        QVERIFY2(preview.startsWith(QLatin1String("/_thumb/m/")),
                 "No medium preview URL for the overlay");
        ++withThumbnails;
    }

    QVERIFY2(withThumbnails > 0, "No image entries were returned, so nothing was checked");
}

void TestThumbnailServer::manyColdMissesAreAllServed()
{
    // More simultaneous misses than there are worker or generator threads. If a
    // miss blocked its worker, this would deadlock or time out — which is exactly
    // what a cold folder page looks like to a user.
    constexpr int Count = 10;

    QList<QTcpSocket*> sockets;
    for (int i = 0; i < Count; ++i) {
        QTcpSocket* socket = connect_();
        QVERIFY(socket);
        socket->write(QByteArray("GET /_thumb/s/Album/photo-")
                      + QByteArray::number(i)
                      + ".png HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        socket->flush();
        sockets.append(socket);
    }

    int served = 0;
    for (QTcpSocket* socket : sockets) {
        Response r;
        if (readResponse(socket, &r, 25'000) && r.status == 200
            && !QImage::fromData(r.body).isNull()) {
            ++served;
        }
        socket->abort();
        socket->deleteLater();
    }

    QCOMPARE(served, Count);
}

void TestThumbnailServer::unknownTierIsRejected()
{
    const Response r = request("GET /_thumb/x/Album/photo-0.png HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");
    QCOMPARE(r.status, 404);
}

void TestThumbnailServer::traversalThroughThumbnailRouteIsRefused()
{
    // The thumbnail route takes a path too, so it needs the same guard as the file
    // route. A separate entry point is an easy place to forget one.
    for (const QByteArray& target : { QByteArray("/_thumb/s/../../secret.png"),
                                      QByteArray("/_thumb/s/%2e%2e/secret.png") }) {
        const Response r = request("GET " + target + " HTTP/1.1\r\n"
                                   "Host: h\r\nConnection: close\r\n\r\n");
        QVERIFY2(r.status == 403,
                 qPrintable(QStringLiteral("%1 returned %2, expected 403")
                                .arg(QString::fromUtf8(target)).arg(r.status)));
    }
}

void TestThumbnailServer::nonImageThumbnailRequestFails()
{
    const Response r = request("GET /_thumb/s/Album/notes.txt HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");
    QCOMPARE(r.status, 404);
}

QTEST_MAIN(TestThumbnailServer)
#include "tst_thumbnailserver.moc"
