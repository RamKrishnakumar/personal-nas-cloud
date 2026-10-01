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
#include "auth/Permissions.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "db/Database.h"
#include "handlers/AuthGate.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/HttpServer.h"

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
        QThread::msleep(1);
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

    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0) {
            out->headers.append({ line.left(colon).toLower(),
                                  line.mid(colon + 1).trimmed() });
        }
    }

    const qint64 expected = out->header("content-length").toLongLong();
    if (!spinUntil([&] { drain(); return buffer.size() - (headerEnd + 4) >= expected; },
                   timeoutMs)) {
        return false;
    }

    out->body = buffer.mid(headerEnd + 4, expected);
    return true;
}

} // namespace

/**
 * Integration tests for handlers::AuthGate and the per-handler permission checks.
 *
 * This is the suite the v0.6 plan summarises as "guest isolation verified", and it
 * is deliberately adversarial: every test asks whether a guest can reach something
 * they were not given, through whichever route looks least guarded. The listing
 * API and the thumbnail route matter most, because both take their target from
 * somewhere the gate cannot see — a query parameter and a path suffix — so each
 * has to check for itself.
 *
 * Library layout:
 *   /Shared/Album      guest may read
 *   /Shared/Private    guest explicitly denied beneath a permitting ancestor
 *   /Family            nobody granted; admin and family reach it by default
 */
class TestAuthzServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // ── Anonymous ────────────────────────────────────────────────────────
    void anonymousNavigationIsRedirectedToLogin();
    void redirectCarriesTheOriginalTarget();
    void anonymousApiCallGets401();
    void anonymousThumbnailGets401();
    void anonymousCannotDownloadAFile();
    void publicPathsStayReachable();
    void healthzIsPublicAndSaysNothingAboutContents();
    void anInvalidCookieIsTreatedAsAnonymous();

    // ── Admin ────────────────────────────────────────────────────────────
    void adminSeesEverything();
    void adminPermissionsAreReportedInTheListing();

    // ── Guest isolation ──────────────────────────────────────────────────
    void guestCannotListTheRoot();
    void guestCanListAGrantedFolder();
    void guestCannotListADeniedSubfolder();
    void guestCannotListAnUngrantedFolder();
    void guestListingHidesEntriesTheyCannotRead();
    void guestCannotDownloadFromADeniedFolder();
    void guestCannotFetchAThumbnailFromADeniedFolder();
    void guestCanFetchAThumbnailFromAGrantedFolder();
    void deniedPathsAnswer404NotForbidden();
    void guestPermissionsAreReportedHonestly();

    // ── Session lifecycle ────────────────────────────────────────────────
    void disablingAnAccountEndsAccessImmediately();
    void logoutEndsAccess();

private:
    Response send(const QByteArray& raw, const QByteArray& token = {});
    QByteArray tokenFor(int userId);

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<db::Database>            m_database;
    std::unique_ptr<auth::UserStore>         m_users;
    std::unique_ptr<auth::SessionStore>      m_sessions;
    std::unique_ptr<auth::PermissionStore>   m_perms;
    std::unique_ptr<auth::LoginRateLimiter>  m_limiter;
    std::unique_ptr<handlers::AuthHandler>   m_auth;
    std::unique_ptr<handlers::FileRouter>    m_router;
    std::unique_ptr<handlers::AuthGate>      m_gate;
    std::unique_ptr<http::HttpServer>        m_server;
    quint16                                  m_port = 0;

    int m_adminId = 0;
    int m_guestId = 0;

    QByteArray m_adminToken;
    QByteArray m_guestToken;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestAuthzServer::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.path() + QStringLiteral("/nas");

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Shared/Album")));
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Shared/Private")));
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Family")));

    const auto writeFile = [](const QString& path, const QByteArray& contents) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(contents);
        return true;
    };

    QVERIFY(writeFile(m_root + QStringLiteral("/Shared/Album/open.txt"), "open"));
    QVERIFY(writeFile(m_root + QStringLiteral("/Shared/Private/secret.txt"), "secret"));
    QVERIFY(writeFile(m_root + QStringLiteral("/Family/holiday.txt"), "family"));
    QVERIFY(writeFile(m_root + QStringLiteral("/top.txt"), "top"));

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp.path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_users = std::make_unique<auth::UserStore>(m_database.get());
    m_users->setIterationsForTesting(1000);

    QString error;
    m_adminId = m_users->create(QStringLiteral("boss"),
                                QStringLiteral("a-good-password"),
                                auth::Role::Admin, &error);
    QVERIFY2(m_adminId > 0, qPrintable(error));

    m_guestId = m_users->create(QStringLiteral("visitor"),
                                QStringLiteral("a-good-password"),
                                auth::Role::Guest, &error);
    QVERIFY2(m_guestId > 0, qPrintable(error));

    m_perms = std::make_unique<auth::PermissionStore>(m_database.get());
    QVERIFY(m_perms->grant(m_guestId, QStringLiteral("/Shared"), auth::PermLevel::Read));
    QVERIFY(m_perms->grant(m_guestId, QStringLiteral("/Shared/Private"),
                           auth::PermLevel::None));

    m_sessions = std::make_unique<auth::SessionStore>(m_database.get());
    m_limiter  = std::make_unique<auth::LoginRateLimiter>();

    m_auth = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());

    m_router = std::make_unique<handlers::FileRouter>(m_root);

    // Note what is *not* called here: setAnonymousAccess(). The gate supplies the
    // identity, and without it the router refuses.
    m_gate = std::make_unique<handlers::AuthGate>(
        m_router.get(), m_auth.get(), m_sessions.get(), m_users.get(), m_perms.get());

    m_server = std::make_unique<http::HttpServer>(m_gate.get());
    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();

    m_adminToken = tokenFor(m_adminId);
    m_guestToken = tokenFor(m_guestId);
    QVERIFY(!m_adminToken.isEmpty());
    QVERIFY(!m_guestToken.isEmpty());
}

void TestAuthzServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_gate.reset();
    m_router.reset();
    m_auth.reset();
    m_limiter.reset();
    m_sessions.reset();
    m_perms.reset();
    m_users.reset();
    m_database.reset();
}

QByteArray TestAuthzServer::tokenFor(int userId)
{
    // Issued directly rather than through /api/login: this suite is about what a
    // session permits, not about how one is obtained.
    return m_sessions->create(userId, auth::SessionStore::DefaultIdleSeconds,
                              QStringLiteral("127.0.0.1"), QStringLiteral("test"));
}

Response TestAuthzServer::send(const QByteArray& raw, const QByteArray& token)
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
        // Inserted after the request line so the header order stays realistic.
        const qsizetype firstBreak = request.indexOf("\r\n");
        request.insert(firstBreak + 2, "Cookie: nd_session=" + token + "\r\n");
    }

    socket.write(request);
    socket.flush();

    readResponse(&socket, &response);
    socket.disconnectFromHost();
    return response;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Anonymous
// ─────────────────────────────────────────────────────────────────────────────

void TestAuthzServer::anonymousNavigationIsRedirectedToLogin()
{
    const Response response = send(
        "GET /Shared HTTP/1.1\r\nHost: nas\r\n"
        "Accept: text/html,application/xhtml+xml\r\n"
        "Sec-Fetch-Mode: navigate\r\n"
        "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 302);
    QVERIFY(response.header("location").startsWith("/login"));
}

void TestAuthzServer::redirectCarriesTheOriginalTarget()
{
    // Otherwise signing in dumps the person on the root instead of the deep link
    // they followed, which is the whole reason for the parameter.
    const Response response = send(
        "GET /Shared/Album?sort=size HTTP/1.1\r\nHost: nas\r\n"
        "Accept: text/html\r\n"
        "Sec-Fetch-Mode: navigate\r\n"
        "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 302);

    const QByteArray location = response.header("location");
    QVERIFY2(location.contains("next="), qPrintable(QString::fromLatin1(location)));
    QVERIFY(location.contains("Shared"));
    QVERIFY(location.contains("Album"));
}

void TestAuthzServer::anonymousApiCallGets401()
{
    // A fetch() must not be redirected to an HTML page: the client would try to
    // parse the login page as JSON and report a syntax error instead of "log in".
    const Response response = send(
        "GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
        "Accept: application/json\r\n"
        "Sec-Fetch-Mode: cors\r\n"
        "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 401);
    QCOMPARE(response.json().value(QStringLiteral("authenticated")).toBool(), false);
    QCOMPARE(response.json().value(QStringLiteral("login")).toString(),
             QStringLiteral("/login"));
}

void TestAuthzServer::anonymousThumbnailGets401()
{
    const Response response = send(
        "GET /_thumb/s/Shared/Album/open.txt HTTP/1.1\r\nHost: nas\r\n"
        "Accept: image/webp,*/*\r\n"
        "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 401);
}

void TestAuthzServer::anonymousCannotDownloadAFile()
{
    // The one that matters most: no file bytes without a session, by any spelling.
    for (const QByteArray& path : { QByteArrayLiteral("/top.txt"),
                                    QByteArrayLiteral("/Shared/Album/open.txt"),
                                    QByteArrayLiteral("/Family/holiday.txt") }) {
        const Response response = send("GET " + path + " HTTP/1.1\r\nHost: nas\r\n"
                                       "Accept: */*\r\n"
                                       "Connection: close\r\n\r\n");

        QVERIFY2(response.status == 401 || response.status == 302,
                 qPrintable(QStringLiteral("%1 returned %2")
                                .arg(QString::fromLatin1(path))
                                .arg(response.status)));
        QVERIFY2(!response.body.contains("open")
                     && !response.body.contains("family")
                     && !response.body.contains("top"),
                 "File contents were served to an anonymous request");
    }
}

void TestAuthzServer::publicPathsStayReachable()
{
    // The login page and its assets must work before anyone has a session, or
    // signing in is impossible.
    for (const QByteArray& path : { QByteArrayLiteral("/login"),
                                    QByteArrayLiteral("/api/session"),
                                    QByteArrayLiteral("/_static/app.js"),
                                    QByteArrayLiteral("/_static/app.css") }) {
        const Response response = send("GET " + path + " HTTP/1.1\r\nHost: nas\r\n"
                                       "Connection: close\r\n\r\n");
        QVERIFY2(response.status == 200,
                 qPrintable(QStringLiteral("%1 returned %2")
                                .arg(QString::fromLatin1(path))
                                .arg(response.status)));
    }
}

void TestAuthzServer::healthzIsPublicAndSaysNothingAboutContents()
{
    const Response response = send("GET /healthz HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 200);
    QCOMPARE(response.body.trimmed(), QByteArray("ok"));

    // It must not leak anything about the library — that is why it is answered in
    // the gate rather than by the file router.
    QVERIFY(!response.body.contains("Shared"));
    QVERIFY(!response.body.contains("Family"));
}

void TestAuthzServer::anInvalidCookieIsTreatedAsAnonymous()
{
    for (const QByteArray& token : { QByteArrayLiteral("not-a-token"),
                                     QByteArrayLiteral("x"),
                                     QByteArrayLiteral("../../etc/passwd") }) {
        const Response response = send(
            "GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n\r\n", token);

        QCOMPARE(response.status, 401);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Admin
// ─────────────────────────────────────────────────────────────────────────────

void TestAuthzServer::adminSeesEverything()
{
    for (const QByteArray& path : { QByteArrayLiteral("/api/list?path=/"),
                                    QByteArrayLiteral("/api/list?path=/Shared"),
                                    QByteArrayLiteral("/api/list?path=/Shared/Private"),
                                    QByteArrayLiteral("/api/list?path=/Family") }) {
        const Response response = send("GET " + path + " HTTP/1.1\r\nHost: nas\r\n"
                                       "Accept: application/json\r\n"
                                       "Connection: close\r\n\r\n", m_adminToken);
        QVERIFY2(response.status == 200,
                 qPrintable(QStringLiteral("%1 returned %2")
                                .arg(QString::fromLatin1(path))
                                .arg(response.status)));
    }

    const Response download = send("GET /Shared/Private/secret.txt HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: */*\r\n"
                                   "Connection: close\r\n\r\n", m_adminToken);
    QCOMPARE(download.status, 200);
    QCOMPARE(download.body, QByteArray("secret"));
}

void TestAuthzServer::adminPermissionsAreReportedInTheListing()
{
    const Response response = send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                                   "Accept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_adminToken);
    QCOMPARE(response.status, 200);

    const QJsonObject perm = response.json().value(QStringLiteral("perm")).toObject();
    QCOMPARE(perm.value(QStringLiteral("read")).toBool(), true);
    QCOMPARE(perm.value(QStringLiteral("write")).toBool(), true);
    QCOMPARE(perm.value(QStringLiteral("delete")).toBool(), true);
    QCOMPARE(perm.value(QStringLiteral("role")).toString(), QStringLiteral("admin"));
    QCOMPARE(perm.value(QStringLiteral("user")).toString(), QStringLiteral("boss"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Guest isolation
// ─────────────────────────────────────────────────────────────────────────────

void TestAuthzServer::guestCannotListTheRoot()
{
    // The guest holds /Shared only. The root was never granted, so it is invisible
    // even though it is the parent of what they can see.
    const Response response = send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                                   "Accept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 404);
    QVERIFY(!response.body.contains("Family"));
    QVERIFY(!response.body.contains("top.txt"));
}

void TestAuthzServer::guestCanListAGrantedFolder()
{
    const Response response = send("GET /api/list?path=/Shared/Album HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 200);
    QVERIFY(response.body.contains("open.txt"));
}

void TestAuthzServer::guestCannotListADeniedSubfolder()
{
    // An explicit none beneath a permitting ancestor. If the ancestor grant leaked
    // down, this is where it would show.
    const Response response = send("GET /api/list?path=/Shared/Private HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 404);
    QVERIFY2(!response.body.contains("secret.txt"),
             "A denied folder's contents were listed");
}

void TestAuthzServer::guestCannotListAnUngrantedFolder()
{
    const Response response = send("GET /api/list?path=/Family HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 404);
    QVERIFY(!response.body.contains("holiday.txt"));
}

void TestAuthzServer::guestListingHidesEntriesTheyCannotRead()
{
    // Listing /Shared, which they may read, must not reveal the denied subfolder.
    // Leaving it in the listing would disclose that it exists and what it is called.
    const Response response = send("GET /api/list?path=/Shared HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 200);

    const QJsonObject body = response.json();
    const QJsonArray entries = body.value(QStringLiteral("entries")).toArray();

    QStringList names;
    for (const QJsonValue& entry : entries)
        names << entry.toObject().value(QStringLiteral("n")).toString();

    QVERIFY2(names.contains(QStringLiteral("Album")),
             qPrintable(QStringLiteral("expected Album, got: %1")
                            .arg(names.join(QStringLiteral(", ")))));
    QVERIFY2(!names.contains(QStringLiteral("Private")),
             "A denied subfolder appeared in the listing");

    // The total must agree with what is actually visible, or the virtual scroller
    // reserves space for rows that are never delivered.
    QCOMPARE(body.value(QStringLiteral("total")).toInt(), names.size());
}

void TestAuthzServer::guestCannotDownloadFromADeniedFolder()
{
    const Response response = send("GET /Shared/Private/secret.txt HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: */*\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 404);
    QVERIFY2(response.body != QByteArray("secret"),
             "A denied file was served to a guest");
}

void TestAuthzServer::guestCannotFetchAThumbnailFromADeniedFolder()
{
    // The thumbnail route takes its target from the path suffix, so the gate cannot
    // check it and the handler must. Without that check a guest could view every
    // photo in the library at 320 px by guessing names.
    const Response response = send("GET /_thumb/s/Shared/Private/secret.txt HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: image/webp\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);

    QCOMPARE(response.status, 404);
}

void TestAuthzServer::guestCanFetchAThumbnailFromAGrantedFolder()
{
    // The permitted case must still reach the handler, or the check is just a wall.
    // open.txt is not an image, so 404 is the honest answer — what matters is that
    // it is the handler's 404 and not the permission check's.
    const Response granted = send("GET /_thumb/s/Shared/Album/open.txt HTTP/1.1\r\n"
                                  "Host: nas\r\nAccept: image/webp\r\n"
                                  "Connection: close\r\n\r\n", m_guestToken);
    QCOMPARE(granted.status, 404);

    // Proved indirectly: the folder itself is reachable, so the grant does work.
    const Response listing = send("GET /api/list?path=/Shared/Album HTTP/1.1\r\n"
                                  "Host: nas\r\nAccept: application/json\r\n"
                                  "Connection: close\r\n\r\n", m_guestToken);
    QCOMPARE(listing.status, 200);
}

void TestAuthzServer::deniedPathsAnswer404NotForbidden()
{
    // 403 would confirm the folder exists, which is what someone probing for other
    // people's albums is trying to learn. 404 tells them nothing.
    for (const QByteArray& path : { QByteArrayLiteral("/Family"),
                                    QByteArrayLiteral("/Shared/Private"),
                                    QByteArrayLiteral("/top.txt") }) {
        const Response response = send("GET " + path + " HTTP/1.1\r\nHost: nas\r\n"
                                       "Accept: */*\r\n"
                                       "Connection: close\r\n\r\n", m_guestToken);

        QVERIFY2(response.status == 404,
                 qPrintable(QStringLiteral("%1 returned %2, which reveals whether it "
                                           "exists")
                                .arg(QString::fromLatin1(path))
                                .arg(response.status)));
    }
}

void TestAuthzServer::guestPermissionsAreReportedHonestly()
{
    const Response response = send("GET /api/list?path=/Shared/Album HTTP/1.1\r\n"
                                   "Host: nas\r\nAccept: application/json\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);
    QCOMPARE(response.status, 200);

    const QJsonObject perm = response.json().value(QStringLiteral("perm")).toObject();
    QCOMPARE(perm.value(QStringLiteral("read")).toBool(), true);
    QCOMPARE(perm.value(QStringLiteral("write")).toBool(), false);
    QCOMPARE(perm.value(QStringLiteral("delete")).toBool(), false);
    QCOMPARE(perm.value(QStringLiteral("role")).toString(), QStringLiteral("guest"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Session lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void TestAuthzServer::disablingAnAccountEndsAccessImmediately()
{
    QString error;
    const int id = m_users->create(QStringLiteral("temp"),
                                   QStringLiteral("a-good-password"),
                                   auth::Role::Admin, &error);
    QVERIFY2(id > 0, qPrintable(error));

    const QByteArray token = tokenFor(id);
    QVERIFY(!token.isEmpty());

    const Response before = send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                                 "Accept: application/json\r\n"
                                 "Connection: close\r\n\r\n", token);
    QCOMPARE(before.status, 200);

    // Turning an account off has to take effect now, not whenever the session
    // happens to expire — twelve hours later is not a revocation.
    m_users->setEnabled(id, false);

    const Response after = send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                                "Accept: application/json\r\n"
                                "Connection: close\r\n\r\n", token);
    QCOMPARE(after.status, 401);

    // And the session is destroyed rather than merely rejected, so re-enabling the
    // account does not silently resurrect it.
    m_users->setEnabled(id, true);
    const Response reEnabled = send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                                    "Accept: application/json\r\n"
                                    "Connection: close\r\n\r\n", token);
    QCOMPARE(reEnabled.status, 401);
}

void TestAuthzServer::logoutEndsAccess()
{
    QString error;
    const int id = m_users->create(QStringLiteral("shortlived"),
                                   QStringLiteral("a-good-password"),
                                   auth::Role::Admin, &error);
    QVERIFY(id > 0);

    const QByteArray token = tokenFor(id);

    QCOMPARE(send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                  "Accept: application/json\r\nConnection: close\r\n\r\n",
                  token).status, 200);

    const Response loggedOut = send("POST /api/logout HTTP/1.1\r\nHost: nas\r\n"
                                    "Content-Length: 0\r\n"
                                    "Connection: close\r\n\r\n", token);
    QCOMPARE(loggedOut.status, 200);

    QCOMPARE(send("GET /api/list?path=/ HTTP/1.1\r\nHost: nas\r\n"
                  "Accept: application/json\r\nConnection: close\r\n\r\n",
                  token).status, 401);
}

QTEST_MAIN(TestAuthzServer)
#include "tst_authzserver.moc"
