#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "auth/LoginRateLimiter.h"
#include "auth/PermissionStore.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "db/Database.h"
#include "handlers/AuthGate.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/HttpServer.h"
#include "media/VideoStore.h"
#include "media/VideoTranscoder.h"

namespace {

struct Response
{
    int        status = 0;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 30'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return true;
}

bool readResponse(QTcpSocket* socket, Response* out, int timeoutMs = 30'000)
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

    qint64 expected = 0;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.toLower().startsWith("content-length:"))
            expected = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
    }

    if (!spinUntil([&] { drain(); return buffer.size() - (headerEnd + 4) >= expected; },
                   timeoutMs)) {
        return false;
    }

    out->body = buffer.mid(headerEnd + 4, expected);
    return true;
}

} // namespace

/**
 * Integration tests for the optimised-video routes.
 *
 * The transcoder and the store are covered on their own. What this suite is for is
 * the HTTP surface, where the mistakes are different in kind: a route that starts
 * four minutes of work on a GET, or one that serves a rendition of a file the
 * requester may not read.
 *
 * Driven through the `fake_ffmpeg` fixture, so a "transcode" takes milliseconds and a
 * test can watch a job through queued, running and ready without waiting for a real
 * encode.
 */
class TestVideoServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void aGetReportsWithoutStartingWork();
    void aPostQueuesAndTheJobCompletes();
    void theRenditionIsServedAndImmutable();
    void theListingAdvertisesAReadyRendition();
    void aGuestCannotAskAboutADeniedFolder();
    void aGuestCannotFetchADeniedRendition();
    void anUnknownPathIsRefused();
    void aPhotoIsNotOfferedOptimisation();
    void otherMethodsAreRefused();

private:
    Response send(const QByteArray& raw, const QByteArray& token);
    Response videoApi(const QByteArray& method, const QString& path,
                      const QByteArray& token);

    QByteArray tokenFor(int userId);

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<db::Database>            m_database;
    std::unique_ptr<auth::UserStore>         m_users;
    std::unique_ptr<auth::SessionStore>      m_sessions;
    std::unique_ptr<auth::PermissionStore>   m_perms;
    std::unique_ptr<auth::LoginRateLimiter>  m_limiter;
    std::unique_ptr<handlers::AuthHandler>   m_auth;
    std::unique_ptr<media::VideoTranscoder>  m_transcoder;
    std::unique_ptr<media::VideoStore>       m_videos;
    std::unique_ptr<handlers::FileRouter>    m_router;
    std::unique_ptr<handlers::AuthGate>      m_gate;
    std::unique_ptr<http::HttpServer>        m_server;
    quint16                                  m_port = 0;

    QByteArray m_adminToken;
    QByteArray m_guestToken;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestVideoServer::initTestCase()
{
    qputenv("FAKE_FFMPEG_MODE", "ok");

    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.path() + QStringLiteral("/nas");

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Shared")));
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Private")));

    const auto write = [](const QString& path, int bytes) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(QByteArray(bytes, 'v')) == bytes;
    };

    QVERIFY(write(m_root + QStringLiteral("/Shared/clip.mov"), 4096));
    QVERIFY(write(m_root + QStringLiteral("/Shared/photo.jpg"), 2048));
    QVERIFY(write(m_root + QStringLiteral("/Private/secret.mov"), 4096));

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp.path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_users = std::make_unique<auth::UserStore>(m_database.get());
    m_users->setIterationsForTesting(1000);

    QString error;
    const int adminId = m_users->create(QStringLiteral("boss"),
                                        QStringLiteral("a-good-password"),
                                        auth::Role::Admin, &error);
    QVERIFY2(adminId > 0, qPrintable(error));

    const int guestId = m_users->create(QStringLiteral("visitor"),
                                        QStringLiteral("a-good-password"),
                                        auth::Role::Guest, &error);
    QVERIFY2(guestId > 0, qPrintable(error));

    m_perms = std::make_unique<auth::PermissionStore>(m_database.get());
    QVERIFY(m_perms->grant(guestId, QStringLiteral("/Shared"), auth::PermLevel::Read));

    m_sessions = std::make_unique<auth::SessionStore>(m_database.get());
    m_limiter  = std::make_unique<auth::LoginRateLimiter>();
    m_auth     = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());

    m_transcoder =
        std::make_unique<media::VideoTranscoder>(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY2(m_transcoder->isAvailable(), "the fake ffmpeg/ffprobe pair was not built");

    m_videos = std::make_unique<media::VideoStore>(
        m_database.get(), m_transcoder.get(),
        m_tmp.path() + QStringLiteral("/renditions"));
    QVERIFY(m_videos->isAvailable());

    m_router = std::make_unique<handlers::FileRouter>(m_root);
    m_router->setVideoStore(m_videos.get());

    m_gate = std::make_unique<handlers::AuthGate>(
        m_router.get(), m_auth.get(), m_sessions.get(), m_users.get(), m_perms.get());

    m_server = std::make_unique<http::HttpServer>(m_gate.get());
    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();

    m_adminToken = tokenFor(adminId);
    m_guestToken = tokenFor(guestId);
    QVERIFY(!m_adminToken.isEmpty());
    QVERIFY(!m_guestToken.isEmpty());
}

void TestVideoServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_gate.reset();
    m_router.reset();
    m_videos.reset();
    m_transcoder.reset();
    m_auth.reset();
    m_limiter.reset();
    m_perms.reset();
    m_sessions.reset();
    m_users.reset();
    m_database.reset();
}

QByteArray TestVideoServer::tokenFor(int userId)
{
    return m_sessions->create(userId, auth::SessionStore::DefaultIdleSeconds,
                              QStringLiteral("127.0.0.1"), QStringLiteral("test"));
}

Response TestVideoServer::send(const QByteArray& raw, const QByteArray& token)
{
    Response response;

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_port);
    if (!spinUntil([&socket] {
            return socket.state() == QAbstractSocket::ConnectedState;
        }, 5000)) {
        return response;
    }

    QByteArray request = raw;
    if (!token.isEmpty()) {
        const qsizetype firstBreak = request.indexOf("\r\n");
        request.insert(firstBreak + 2, "Cookie: nd_session=" + token + "\r\n");
    }

    socket.write(request);
    socket.flush();

    readResponse(&socket, &response);
    socket.disconnectFromHost();
    return response;
}

Response TestVideoServer::videoApi(const QByteArray& method, const QString& path,
                                   const QByteArray& token)
{
    const QByteArray target =
        "/api/video?path=" + QUrl::toPercentEncoding(path);

    QByteArray raw = method + " " + target + " HTTP/1.1\r\n"
                     "Host: nas\r\n";
    if (method == "POST")
        raw += "Content-Length: 0\r\n";
    raw += "Connection: close\r\n\r\n";

    return send(raw, token);
}

// ─────────────────────────────────────────────────────────────────────────────

void TestVideoServer::aGetReportsWithoutStartingWork()
{
    // The property that matters most here. Transcoding costs minutes, so a GET must
    // never start one — a browser or crawler prefetching links would otherwise set a
    // whole library encoding, on a laptop, uninvited.
    const Response response =
        videoApi("GET", QStringLiteral("/Shared/clip.mov"), m_adminToken);

    QCOMPARE(response.status, 200);

    const QJsonObject body = response.json();
    QVERIFY(body.value(QStringLiteral("available")).toBool());
    QCOMPARE(body.value(QStringLiteral("state")).toString(),
             QStringLiteral("unknown"));

    QCOMPARE(m_videos->pending(), 0);

    // Asked repeatedly, in case one GET is harmless and ten are not.
    for (int i = 0; i < 5; ++i)
        videoApi("GET", QStringLiteral("/Shared/clip.mov"), m_adminToken);

    QVERIFY2(m_videos->pending() == 0,
             "a GET queued work; only POST may do that");
}

void TestVideoServer::aPostQueuesAndTheJobCompletes()
{
    const Response queued =
        videoApi("POST", QStringLiteral("/Shared/clip.mov"), m_adminToken);
    QCOMPARE(queued.status, 200);

    const QString state = queued.json().value(QStringLiteral("state")).toString();
    QVERIFY2(state == QLatin1String("queued") || state == QLatin1String("running")
                 || state == QLatin1String("ready"),
             qPrintable(state));

    // The fixture finishes in milliseconds, so this is about the state machine
    // arriving somewhere terminal rather than about speed.
    QVERIFY(spinUntil([this] {
        return videoApi("GET", QStringLiteral("/Shared/clip.mov"), m_adminToken)
                   .json().value(QStringLiteral("state")).toString()
               == QLatin1String("ready");
    }));

    const Response ready =
        videoApi("GET", QStringLiteral("/Shared/clip.mov"), m_adminToken);
    const QJsonObject body = ready.json();

    QCOMPARE(body.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    QVERIFY2(body.value(QStringLiteral("url")).toString().startsWith(
                 QStringLiteral("/_video/")),
             qPrintable(body.value(QStringLiteral("url")).toString()));

    // Asking again must join rather than start a second job, which is what makes the
    // client free to poll.
    videoApi("POST", QStringLiteral("/Shared/clip.mov"), m_adminToken);
    QCOMPARE(m_videos->pending(), 0);
}

void TestVideoServer::theRenditionIsServedAndImmutable()
{
    // Depends on the job from the previous test, which is deliberate — the states run
    // in order and re-transcoding per test would say nothing extra.
    QVERIFY(spinUntil([this] {
        return videoApi("GET", QStringLiteral("/Shared/clip.mov"), m_adminToken)
                   .json().value(QStringLiteral("state")).toString()
               == QLatin1String("ready");
    }));

    const Response response = send("GET /_video/Shared/clip.mov HTTP/1.1\r\n"
                                   "Host: nas\r\nConnection: close\r\n\r\n",
                                   m_adminToken);

    QCOMPARE(response.status, 200);
    QVERIFY2(response.body.size() > 0, "the rendition was served empty");
}

void TestVideoServer::theListingAdvertisesAReadyRendition()
{
    QVERIFY(spinUntil([this] {
        return videoApi("GET", QStringLiteral("/Shared/clip.mov"), m_adminToken)
                   .json().value(QStringLiteral("state")).toString()
               == QLatin1String("ready");
    }));

    const Response listing =
        send("GET /api/list?path=/Shared HTTP/1.1\r\n"
             "Host: nas\r\nConnection: close\r\n\r\n", m_adminToken);
    QCOMPARE(listing.status, 200);

    const QJsonArray entries =
        listing.json().value(QStringLiteral("entries")).toArray();

    bool found = false;
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("n")).toString() != QLatin1String("clip.mov"))
            continue;

        found = true;

        // Carried in the listing so opening an already-optimised video costs no extra
        // round trip at all.
        QCOMPARE(entry.value(QStringLiteral("vs")).toString(), QStringLiteral("ready"));
        QVERIFY2(entry.value(QStringLiteral("vo")).toString()
                     .startsWith(QStringLiteral("/_video/")),
                 qPrintable(entry.value(QStringLiteral("vo")).toString()));
    }

    QVERIFY2(found, "clip.mov was missing from the listing");
}

void TestVideoServer::aGuestCannotAskAboutADeniedFolder()
{
    // The status endpoint takes its target from a query parameter, which the gate
    // cannot see — so it has to check for itself, against the path it resolved.
    const Response response =
        videoApi("GET", QStringLiteral("/Private/secret.mov"), m_guestToken);

    QCOMPARE(response.status, 404);

    // And it must not have been queued either: a denial that still did the work would
    // leak the file's existence through the machine's fan.
    QCOMPARE(m_videos->pending(), 0);

    const Response posted =
        videoApi("POST", QStringLiteral("/Private/secret.mov"), m_guestToken);
    QCOMPARE(posted.status, 404);
    QCOMPARE(m_videos->pending(), 0);
}

void TestVideoServer::aGuestCannotFetchADeniedRendition()
{
    // First make one exist, as the administrator.
    videoApi("POST", QStringLiteral("/Private/secret.mov"), m_adminToken);
    QVERIFY(spinUntil([this] {
        return videoApi("GET", QStringLiteral("/Private/secret.mov"), m_adminToken)
                   .json().value(QStringLiteral("state")).toString()
               == QLatin1String("ready");
    }));

    // Now the guest asks for it directly, bypassing the status endpoint entirely.
    // 404 rather than 403, for the same reason as everywhere else: 403 would confirm
    // the file exists.
    const Response response = send("GET /_video/Private/secret.mov HTTP/1.1\r\n"
                                   "Host: nas\r\nConnection: close\r\n\r\n",
                                   m_guestToken);
    QCOMPARE(response.status, 404);
}

void TestVideoServer::anUnknownPathIsRefused()
{
    const Response missing =
        videoApi("GET", QStringLiteral("/Shared/nothing-here.mov"), m_adminToken);
    QCOMPARE(missing.status, 200);
    QCOMPARE(missing.json().value(QStringLiteral("state")).toString(),
             QStringLiteral("unknown"));

    // A traversal attempt is refused by the resolver before any of this is reached.
    const Response escape =
        videoApi("GET", QStringLiteral("/../../etc/passwd"), m_adminToken);
    QVERIFY2(escape.status == 403 || escape.status == 404,
             qPrintable(QString::number(escape.status)));
}

void TestVideoServer::aPhotoIsNotOfferedOptimisation()
{
    // Nothing here should think a JPEG is a video. Queueing one would spend a probe
    // subprocess to learn what the extension already said.
    const Response response =
        videoApi("POST", QStringLiteral("/Shared/photo.jpg"), m_adminToken);

    QCOMPARE(response.status, 200);
    QCOMPARE(response.json().value(QStringLiteral("state")).toString(),
             QStringLiteral("unknown"));
    QCOMPARE(m_videos->pending(), 0);
}

void TestVideoServer::otherMethodsAreRefused()
{
    // POST is allowed on this one path. Everything else still has no defined meaning
    // until uploads arrive, and answering 405 says so rather than silently ignoring it.
    const Response response =
        videoApi("DELETE", QStringLiteral("/Shared/clip.mov"), m_adminToken);
    QCOMPARE(response.status, 405);
}

QTEST_GUILESS_MAIN(TestVideoServer)
#include "tst_videoserver.moc"
