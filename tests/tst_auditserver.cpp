#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "auth/LoginRateLimiter.h"
#include "auth/PermissionStore.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "db/AuditLog.h"
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
};

/** One audit row, flattened to what the assertions care about. */
struct Row
{
    QString action;
    QVariant userId;     ///< Null when nobody was identified.
    QString path;
    qint64  bytes  = -1;
    int     status = 0;
    QString address;
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
 * Integration tests for the audit log's call sites — step 16 of the v0.6 plan.
 *
 * tst_auditlog covers the store. This suite covers the decisions about *what* is
 * written, which live in the handlers and are the part that can quietly rot: a
 * missing call site produces no error, no failing assertion anywhere else, and a log
 * that looks like it is working right up until someone needs it.
 *
 * The test that matters most is the one about an unknown username. People type
 * passwords into the username box, so recording whatever was typed would put
 * plaintext passwords in the database. Resolving the username to an account id first
 * means that cannot happen, and if anyone later "improves" this by storing the raw
 * attempt, that test is what says no.
 */
class TestAuditServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();

    // ── Authentication ───────────────────────────────────────────────────
    void aSuccessfulLoginIsRecordedAgainstTheAccount();
    void aFailedLoginAgainstAKnownAccountNamesIt();
    void aFailedLoginAgainstAnUnknownUsernameNamesNobody();
    void aThrottledAttemptIsRecordedWithItsOwnStatus();
    void aSignOutIsRecordedBeforeTheSessionIsDestroyed();

    // ── Serving ──────────────────────────────────────────────────────────
    void openingAFolderIsRecordedOncePerVisit();
    void aDownloadRecordsThePathAndTheSize();
    void aRefusedPathIsRecordedEvenThoughTheClientSees404();
    void thumbnailsAreNotRecorded();
    void anAnonymousRequestRecordsNothing();

private:
    Response send(const QByteArray& raw, const QByteArray& token = {});
    Response login(const QString& username, const QString& password);
    QByteArray tokenFor(int userId);

    /** Every row, oldest first. */
    QList<Row> rows();

    /** Rows with the given action, oldest first. */
    QList<Row> rows(const QString& action);

    void clearAudit();

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<db::Database>           m_database;
    std::unique_ptr<db::AuditLog>           m_audit;
    std::unique_ptr<auth::UserStore>        m_users;
    std::unique_ptr<auth::SessionStore>     m_sessions;
    std::unique_ptr<auth::PermissionStore>  m_perms;
    std::unique_ptr<auth::LoginRateLimiter> m_limiter;
    std::unique_ptr<handlers::AuthHandler>  m_auth;
    std::unique_ptr<handlers::FileRouter>   m_router;
    std::unique_ptr<handlers::AuthGate>     m_gate;
    std::unique_ptr<http::HttpServer>       m_server;
    quint16                                 m_port = 0;

    int m_adminId = 0;
    int m_guestId = 0;

    QByteArray m_adminToken;
    QByteArray m_guestToken;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestAuditServer::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.path() + QStringLiteral("/nas");

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Shared")));
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Private")));

    const auto writeFile = [](const QString& path, const QByteArray& contents) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(contents) == contents.size();
    };

    QVERIFY(writeFile(m_root + QStringLiteral("/Shared/open.txt"), "0123456789"));
    QVERIFY(writeFile(m_root + QStringLiteral("/Private/secret.txt"), "secret"));

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp.path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_audit = std::make_unique<db::AuditLog>(m_database.get());

    m_users = std::make_unique<auth::UserStore>(m_database.get());

    // 1000 instead of 600,000. Verification reads the factor from the stored hash, so
    // signing in is fast too — twelve logins at the real factor would be 17 seconds.
    m_users->setIterationsForTesting(1000);

    QString error;
    m_adminId = m_users->create(QStringLiteral("boss"), QStringLiteral("a-good-password"),
                                auth::Role::Admin, &error);
    QVERIFY2(m_adminId > 0, qPrintable(error));

    m_guestId = m_users->create(QStringLiteral("visitor"),
                                QStringLiteral("a-good-password"),
                                auth::Role::Guest, &error);
    QVERIFY2(m_guestId > 0, qPrintable(error));

    m_perms = std::make_unique<auth::PermissionStore>(m_database.get());
    QVERIFY(m_perms->grant(m_guestId, QStringLiteral("/Shared"), auth::PermLevel::Read));

    m_sessions = std::make_unique<auth::SessionStore>(m_database.get());
    m_limiter  = std::make_unique<auth::LoginRateLimiter>();

    m_auth = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());
    m_auth->setAuditLog(m_audit.get());

    m_router = std::make_unique<handlers::FileRouter>(m_root);
    m_router->setAuditLog(m_audit.get());

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

void TestAuditServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_gate.reset();
    m_router.reset();
    m_auth.reset();
    m_limiter.reset();
    m_perms.reset();
    m_sessions.reset();
    m_users.reset();
    m_audit.reset();
    m_database.reset();
}

void TestAuditServer::init()
{
    // Emptied before each test rather than after, so a failure leaves its rows behind
    // to be looked at.
    clearAudit();
}

void TestAuditServer::clearAudit()
{
    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    QVERIFY2(query.exec(QStringLiteral("DELETE FROM audit")),
             qPrintable(query.lastError().text()));
}

QList<Row> TestAuditServer::rows()
{
    QList<Row> result;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (!query.exec(QStringLiteral(
            "SELECT action, user_id, path, bytes, status, ip FROM audit ORDER BY id"))) {
        return result;
    }

    while (query.next()) {
        Row row;
        row.action  = query.value(0).toString();
        row.userId  = query.value(1);
        row.path    = query.value(2).toString();
        row.bytes   = query.value(3).isNull() ? -1 : query.value(3).toLongLong();
        row.status  = query.value(4).toInt();
        row.address = query.value(5).toString();
        result.append(row);
    }

    return result;
}

QList<Row> TestAuditServer::rows(const QString& action)
{
    QList<Row> matching;
    for (const Row& row : rows()) {
        if (row.action == action)
            matching.append(row);
    }
    return matching;
}

QByteArray TestAuditServer::tokenFor(int userId)
{
    return m_sessions->create(userId, auth::SessionStore::DefaultIdleSeconds,
                              QStringLiteral("127.0.0.1"), QStringLiteral("test"));
}

Response TestAuditServer::send(const QByteArray& raw, const QByteArray& token)
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

Response TestAuditServer::login(const QString& username, const QString& password)
{
    QJsonObject object;
    object[QStringLiteral("username")] = username;
    object[QStringLiteral("password")] = password;

    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QByteArray raw = "POST /api/login HTTP/1.1\r\n"
                     "Host: nas\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n";
    raw += body;

    return send(raw);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Authentication
// ─────────────────────────────────────────────────────────────────────────────

void TestAuditServer::aSuccessfulLoginIsRecordedAgainstTheAccount()
{
    const Response response = login(QStringLiteral("boss"),
                                    QStringLiteral("a-good-password"));
    QCOMPARE(response.status, 200);

    const QList<Row> entries = rows(QStringLiteral("login"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().userId.toInt(), m_adminId);
    QCOMPARE(entries.first().status, 200);
    QVERIFY(!entries.first().address.isEmpty());
}

void TestAuditServer::aFailedLoginAgainstAKnownAccountNamesIt()
{
    // Naming the account is the useful half: "someone is trying to get into boss"
    // is actionable, where an anonymous failure count is not.
    const Response response = login(QStringLiteral("boss"),
                                    QStringLiteral("the-wrong-password"));
    QCOMPARE(response.status, 401);

    const QList<Row> entries = rows(QStringLiteral("login_fail"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().userId.toInt(), m_adminId);
    QCOMPARE(entries.first().status, 401);
}

void TestAuditServer::aFailedLoginAgainstAnUnknownUsernameNamesNobody()
{
    // The privacy guard. A username nobody has is very often a password typed into
    // the wrong box, so nothing about what was typed may reach the database. The row
    // still exists — the address and the count are what make probing visible — but it
    // identifies no account, because there is no account to identify.
    const QString looksLikeAPassword = QStringLiteral("Hunter2!correct-horse");

    const Response response = login(looksLikeAPassword,
                                    QStringLiteral("a-good-password"));
    QCOMPARE(response.status, 401);

    const QList<Row> entries = rows(QStringLiteral("login_fail"));
    QCOMPARE(entries.size(), 1);
    QVERIFY2(entries.first().userId.isNull(),
             "an attempt on a username that does not exist must name no account");

    // Belt and braces: whatever was typed must not appear anywhere in the row, in any
    // column. The path column is the one somebody would reach for to "improve" this.
    for (const Row& row : rows()) {
        QVERIFY2(!row.path.contains(looksLikeAPassword, Qt::CaseInsensitive),
                 "the attempted username was stored; it may be someone's password");
        QVERIFY(!row.address.contains(looksLikeAPassword, Qt::CaseInsensitive));
    }
}

void TestAuditServer::aThrottledAttemptIsRecordedWithItsOwnStatus()
{
    // A distinct status rather than a distinct action: the vocabulary stays as the
    // plan specifies it, and 429 still says plainly that this one never reached the
    // password check.
    const QString username = QStringLiteral("visitor");

    // The limiter allows five failures per username per fifteen minutes.
    for (int attempt = 0; attempt < 5; ++attempt) {
        const Response rejected = login(username, QStringLiteral("wrong-password"));
        QCOMPARE(rejected.status, 401);
    }

    const Response throttled = login(username, QStringLiteral("wrong-password"));
    QCOMPARE(throttled.status, 429);

    const QList<Row> entries = rows(QStringLiteral("login_fail"));
    QCOMPARE(entries.size(), 6);

    // Five rejections then one refusal, in that order.
    for (int i = 0; i < 5; ++i)
        QCOMPARE(entries.at(i).status, 401);
    QCOMPARE(entries.at(5).status, 429);

    // Every one of them names the account, including the throttled attempt — which it
    // can, because the lookup happens before the limiter's verdict is acted on.
    for (const Row& row : entries)
        QCOMPARE(row.userId.toInt(), m_guestId);
}

void TestAuditServer::aSignOutIsRecordedBeforeTheSessionIsDestroyed()
{
    // Ordering is the whole test. Once the session row is gone there is no way left
    // to say whose sign-out it was, so a logout audited after the destroy would
    // record an anonymous event every time.
    const QByteArray token = tokenFor(m_adminId);
    QVERIFY(!token.isEmpty());

    const Response response = send("POST /api/logout HTTP/1.1\r\nHost: nas\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n", token);
    QCOMPARE(response.status, 200);

    const QList<Row> entries = rows(QStringLiteral("logout"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().userId.toInt(), m_adminId);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Serving
// ─────────────────────────────────────────────────────────────────────────────

void TestAuditServer::openingAFolderIsRecordedOncePerVisit()
{
    const Response first = send("GET /api/list?path=/Shared&offset=0&limit=200 "
                               "HTTP/1.1\r\nHost: nas\r\nConnection: close\r\n\r\n",
                               m_adminToken);
    QCOMPARE(first.status, 200);
    QCOMPARE(rows(QStringLiteral("list")).size(), 1);
    QCOMPARE(rows(QStringLiteral("list")).first().path, QStringLiteral("/Shared"));

    // Later pages of the same folder say nothing new. A five-thousand-file folder
    // issues twenty-five of these while scrolling, and twenty-four of them would be
    // noise that buries every other entry in the table.
    const Response second = send("GET /api/list?path=/Shared&offset=200&limit=200 "
                                "HTTP/1.1\r\nHost: nas\r\nConnection: close\r\n\r\n",
                                m_adminToken);
    QCOMPARE(second.status, 200);
    QCOMPARE(rows(QStringLiteral("list")).size(), 1);
}

void TestAuditServer::aDownloadRecordsThePathAndTheSize()
{
    const Response response = send("GET /Shared/open.txt HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n", m_adminToken);
    QCOMPARE(response.status, 200);

    const QList<Row> entries = rows(QStringLiteral("download"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().userId.toInt(), m_adminId);
    QCOMPARE(entries.first().path, QStringLiteral("/Shared/open.txt"));
    QCOMPARE(entries.first().status, 200);

    // The size of the file asked for, not a count of bytes delivered. A Range request
    // or an abandoned transfer moves fewer, and the row is written before the transfer
    // starts so that it exists even when the client vanishes.
    QCOMPARE(entries.first().bytes, 10);
}

void TestAuditServer::aRefusedPathIsRecordedEvenThoughTheClientSees404()
{
    // The log is the one place the distinction survives. The guest is told only that
    // there is nothing there — 403 would confirm the folder exists — while the owner
    // of the machine can still see that someone went looking for it.
    const Response response = send("GET /Private/secret.txt HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n", m_guestToken);
    QCOMPARE(response.status, 404);

    const QList<Row> entries = rows(QStringLiteral("download"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().userId.toInt(), m_guestId);
    QCOMPARE(entries.first().path, QStringLiteral("/Private/secret.txt"));
    QCOMPARE(entries.first().status, 404);
    QCOMPARE(entries.first().bytes, -1);     // Nothing was sent.
}

void TestAuditServer::thumbnailsAreNotRecorded()
{
    // Deliberate, and the reason the table stays readable. One folder view fires
    // dozens of these; recording them would make the log almost entirely about the
    // one action nobody audits.
    const Response response = send("GET /_thumb/s/Shared/open.txt HTTP/1.1\r\n"
                                   "Host: nas\r\nConnection: close\r\n\r\n",
                                   m_adminToken);

    // Whatever it answers — there is no thumbnail cache wired into this suite, so a
    // 404 is expected — it must not have written a row.
    QVERIFY(response.status != 0);
    QCOMPARE(rows().size(), 0);
}

void TestAuditServer::anAnonymousRequestRecordsNothing()
{
    // The gate turns these away before any handler runs, so there is nothing to
    // record and no user to record it against. That is the right trade: every
    // bookmark opened with an expired session is one of these, so logging them would
    // fill the table with an event that means only "a cookie timed out".
    const Response response = send("GET /Shared/open.txt HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");
    QVERIFY(response.status == 302 || response.status == 401);
    QCOMPARE(rows().size(), 0);
}

QTEST_GUILESS_MAIN(TestAuditServer)
#include "tst_auditserver.moc"
