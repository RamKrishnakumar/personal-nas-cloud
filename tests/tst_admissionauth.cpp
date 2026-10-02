#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
#include "http/HardwareTier.h"
#include "http/HttpServer.h"

namespace {

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 10'000)
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
 * Bytes a test client will buffer before it stops reading from the kernel.
 *
 * Without a cap, leaving the body unread does not hold the transfer: QTcpSocket
 * keeps draining the kernel buffer into memory of its own accord whenever the event
 * loop runs, and these tests spin the loop constantly. A 6 MB "held" file would then
 * finish being sent, release its slot, and the next request would be admitted — a
 * flake that scales with how fast the machine is, and that looks like a slot leak.
 *
 * Capping it means the kernel buffer fills, TCP applies backpressure, and the server
 * stops at its own high-water mark with the slot genuinely held. Comfortably above
 * any response header block, so reading a status line never blocks.
 */
constexpr int ClientReadBufferBytes = 64 * 1024;

/**
 * Reads the status line and headers, leaving the body unread on purpose.
 *
 * That is what holds the slot: the server fills its write buffer, stops, and the
 * transfer stays in progress for as long as the test wants it to.
 */
bool readStatus(QTcpSocket* socket, int* status, QByteArray* head, int timeoutMs = 10'000)
{
    QByteArray buffer;
    const bool complete = spinUntil([&] {
        if (socket->bytesAvailable() > 0)
            buffer += socket->readAll();
        return buffer.contains("\r\n\r\n");
    }, timeoutMs);

    if (!complete)
        return false;

    *head = buffer.left(buffer.indexOf("\r\n\r\n"));
    const QList<QByteArray> parts = head->split('\n').first().simplified().split(' ');
    if (parts.size() < 2)
        return false;
    *status = parts.at(1).toInt();
    return true;
}

} // namespace

/**
 * Admission control with identities attached — step 15 of the v0.6 plan.
 *
 * The bookkeeping is already covered by tst_admissioncontrol, and the socket
 * behaviour by tst_admissionserver. Neither could cover what this suite is for,
 * because until step 15 nothing told AdmissionControl who was asking: slots were
 * keyed on the peer address and the role was never passed at all, so the reserved
 * slot existed in the arithmetic and was unreachable in practice.
 *
 * What is actually under test is the wiring between three pieces that each looked
 * correct alone — the gate that knows the session, the connection that asks for the
 * slot, and the controller that grants it.
 *
 * Every client here connects from 127.0.0.1, which is the point: under the old key
 * they were one client, and the household behind one NAT that this models was
 * limited as though it were one person.
 *
 * Three shared slots, one admin reserve. Three rather than the production two so
 * the per-session limit of two bites before the global ceiling — otherwise a
 * refusal would not say which limit produced it.
 */
class TestAdmissionAuth : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void threeSessionsFromOneAddressEachGetASlot();
    void aFourthMemberIsRefusedOnceThePoolIsFull();
    void adminGetsASlotWhenTheSharedPoolIsFull();
    void theReserveIsOneSlotAndNotAFreePass();
    void aSeekFromTheSameSessionOnANewConnectionIsFree();
    void anAnonymousTransferNeverReachesAdmission();

private:
    /** Opens a transfer and returns the socket still holding it, or nullptr. */
    QTcpSocket* hold(const QByteArray& file, const QByteArray& token, int* status);

    QByteArray tokenFor(int userId);

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<db::Database>           m_database;
    std::unique_ptr<auth::UserStore>        m_users;
    std::unique_ptr<auth::SessionStore>     m_sessions;
    std::unique_ptr<auth::PermissionStore>  m_perms;
    std::unique_ptr<auth::LoginRateLimiter> m_limiter;
    std::unique_ptr<handlers::AuthHandler>  m_auth;
    std::unique_ptr<handlers::FileRouter>   m_router;
    std::unique_ptr<handlers::AuthGate>     m_gate;
    std::unique_ptr<http::HttpServer>       m_server;
    quint16                                 m_port = 0;

    int m_familyId = 0;
    int m_adminId  = 0;

    /// Three separate sessions for one ordinary account, standing in for three
    /// devices, plus two for the administrator.
    QByteArray m_family[3];
    QByteArray m_admin[2];

    /// Sockets opened by a test, closed by cleanup() whatever the outcome. Leaving
    /// one behind would hold a slot into the next test and fail it misleadingly.
    QList<QTcpSocket*> m_open;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionAuth::initTestCase()
{
    QVERIFY2(m_tmp.isValid(), qPrintable(m_tmp.errorString()));
    m_root = m_tmp.path() + QStringLiteral("/nas");
    QVERIFY(QDir().mkpath(m_root));

    // Over the 2 MB slot threshold, and more than the socket buffers plus the
    // connection's high-water mark can swallow, so the transfer is still live while
    // the test looks at it.
    const QByteArray payload(6 * 1024 * 1024, 'x');
    for (const char* name : { "a.mp4", "b.mp4", "c.mp4", "d.mp4", "e.mp4" }) {
        QFile file(m_root + QLatin1Char('/') + QLatin1String(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(payload), payload.size());
    }

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp.path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_users = std::make_unique<auth::UserStore>(m_database.get());
    m_users->setIterationsForTesting(1000);   // 600k would add seconds for nothing.

    QString error;
    m_familyId = m_users->create(QStringLiteral("household"),
                                 QStringLiteral("a-good-password"),
                                 auth::Role::Family, &error);
    QVERIFY2(m_familyId > 0, qPrintable(error));

    m_adminId = m_users->create(QStringLiteral("owner"),
                                QStringLiteral("a-good-password"),
                                auth::Role::Admin, &error);
    QVERIFY2(m_adminId > 0, qPrintable(error));

    m_sessions = std::make_unique<auth::SessionStore>(m_database.get());
    m_perms    = std::make_unique<auth::PermissionStore>(m_database.get());
    m_limiter  = std::make_unique<auth::LoginRateLimiter>();

    m_auth = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());

    m_router = std::make_unique<handlers::FileRouter>(m_root);
    m_gate   = std::make_unique<handlers::AuthGate>(
        m_router.get(), m_auth.get(), m_sessions.get(), m_users.get(), m_perms.get());

    m_server = std::make_unique<http::HttpServer>(m_gate.get());
    m_server->setHardwareProfile(
        http::profileWithStreamSlots(http::detectHardwareProfile(), 3));

    http::ConnectionLimits limits = m_server->connectionLimits();
    limits.admissionWaitMs  = 600;    // Short, so a refusal arrives promptly.
    limits.admissionRetryMs = 50;
    limits.stallTimeoutMs   = 20'000; // Long, so a held transfer is not abandoned.
    m_server->setConnectionLimits(limits);

    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();
    QVERIFY(m_port != 0);

    for (QByteArray& token : m_family) {
        token = tokenFor(m_familyId);
        QVERIFY(!token.isEmpty());
    }
    for (QByteArray& token : m_admin) {
        token = tokenFor(m_adminId);
        QVERIFY(!token.isEmpty());
    }

    // Three sessions for one account must be three distinct identities, or the
    // first test would pass for the wrong reason.
    QVERIFY(m_family[0] != m_family[1]);
    QVERIFY(m_family[1] != m_family[2]);
}

void TestAdmissionAuth::cleanupTestCase()
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
    m_database.reset();
}

void TestAdmissionAuth::cleanup()
{
    for (QTcpSocket* socket : m_open) {
        socket->abort();
        delete socket;
    }
    m_open.clear();

    // Waited for rather than assumed: a slot released on the next test's clock
    // would make that test fail for a reason it has nothing to do with.
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 0; }));
}

QByteArray TestAdmissionAuth::tokenFor(int userId)
{
    return m_sessions->create(userId, auth::SessionStore::DefaultIdleSeconds,
                              QStringLiteral("127.0.0.1"), QStringLiteral("test"));
}

QTcpSocket* TestAdmissionAuth::hold(const QByteArray& file, const QByteArray& token,
                                    int* status)
{
    auto* socket = new QTcpSocket;
    socket->setReadBufferSize(ClientReadBufferBytes);
    socket->connectToHost(QHostAddress::LocalHost, m_port);
    if (!spinUntil([socket] {
            return socket->state() == QAbstractSocket::ConnectedState;
        }, 5000)) {
        delete socket;
        return nullptr;
    }

    m_open.append(socket);

    QByteArray request = "GET /" + file + " HTTP/1.1\r\nHost: nas\r\n";
    if (!token.isEmpty())
        request += "Cookie: nd_session=" + token + "\r\n";
    request += "\r\n";

    socket->write(request);
    socket->flush();

    QByteArray head;
    // 8 s: a refusal has to outlast admissionWaitMs plus the retry interval.
    if (!readStatus(socket, status, &head, 8000))
        return nullptr;

    return socket;
}

// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionAuth::threeSessionsFromOneAddressEachGetASlot()
{
    // The reason for the switch. All three connect from 127.0.0.1, so under the old
    // peer-address key they were one client and the third would have been refused by
    // the per-client limit of two — with the pool only two-thirds full. That is what
    // a family sharing a router used to get.
    const QByteArray files[3] = { "a.mp4", "b.mp4", "c.mp4" };

    for (int i = 0; i < 3; ++i) {
        int status = 0;
        QVERIFY2(hold(files[i], m_family[i], &status) != nullptr,
                 qPrintable(QStringLiteral("session %1 got no response").arg(i)));
        QVERIFY2(status == 200,
                 qPrintable(QStringLiteral("session %1 was answered %2; slots are still "
                                           "keyed on the address, not the session")
                                .arg(i).arg(status)));
    }

    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 3; }));
}

void TestAdmissionAuth::aFourthMemberIsRefusedOnceThePoolIsFull()
{
    // The other half of the test above: keying per session widens the limit, it does
    // not remove it. Without this, a bug that admitted everyone unconditionally
    // would pass the previous test.
    const QByteArray files[3] = { "a.mp4", "b.mp4", "c.mp4" };
    for (int i = 0; i < 3; ++i) {
        int status = 0;
        QVERIFY(hold(files[i], m_family[i], &status) != nullptr);
        QCOMPARE(status, 200);
    }
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 3; }));

    // A fourth session for the same ordinary account, on a file nobody is streaming,
    // so no slot can be shared.
    const QByteArray fourth = tokenFor(m_familyId);
    QVERIFY(!fourth.isEmpty());

    int status = 0;
    QVERIFY(hold("d.mp4", fourth, &status) != nullptr);
    QCOMPARE(status, 503);
}

void TestAdmissionAuth::adminGetsASlotWhenTheSharedPoolIsFull()
{
    // The step 15 verification criterion. The owner of the machine must not be
    // locked out of their own storage by everyone else using it.
    const QByteArray files[3] = { "a.mp4", "b.mp4", "c.mp4" };
    for (int i = 0; i < 3; ++i) {
        int status = 0;
        QVERIFY(hold(files[i], m_family[i], &status) != nullptr);
        QCOMPARE(status, 200);
    }
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 3; }));

    // Proved full first, with an ordinary session on the very file the administrator
    // is about to ask for. Without this half, the 200 below would be consistent with
    // the pool simply having had room.
    int refused = 0;
    const QByteArray spare = tokenFor(m_familyId);
    QVERIFY(hold("d.mp4", spare, &refused) != nullptr);
    QCOMPARE(refused, 503);

    int granted = 0;
    QVERIFY(hold("d.mp4", m_admin[0], &granted) != nullptr);
    QVERIFY2(granted == 200,
             qPrintable(QStringLiteral("the administrator was answered %1 for a file an "
                                       "ordinary session was just refused; the reserve "
                                       "is not reachable").arg(granted)));

    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 4; }));
}

void TestAdmissionAuth::theReserveIsOneSlotAndNotAFreePass()
{
    // The dangerous misreading of the reserve. If being an administrator simply
    // lifted the ceiling, a laptop signed in as the owner could start six transfers
    // and make the machine unusable — which is the exact problem admission control
    // was added to prevent.
    const QByteArray files[3] = { "a.mp4", "b.mp4", "c.mp4" };
    for (int i = 0; i < 3; ++i) {
        int status = 0;
        QVERIFY(hold(files[i], m_family[i], &status) != nullptr);
        QCOMPARE(status, 200);
    }

    int first = 0;
    QVERIFY(hold("d.mp4", m_admin[0], &first) != nullptr);
    QCOMPARE(first, 200);                       // The reserve, now spent.
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 4; }));

    int second = 0;
    QVERIFY(hold("e.mp4", m_admin[1], &second) != nullptr);
    QVERIFY2(second == 503,
             qPrintable(QStringLiteral("a second administrator stream past the reserve "
                                       "was answered %1; the reserve is behaving as an "
                                       "exemption").arg(second)));
}

void TestAdmissionAuth::aSeekFromTheSameSessionOnANewConnectionIsFree()
{
    // The deadlock the reference counting exists to prevent, retested because the
    // switch could have reintroduced it: a player closes its connection and reopens
    // with a Range header, and the slot is only reused if the identity is the same
    // across two connections. The peer address used to give that for free. A session
    // cookie has to be read, matched and formed identically both times, and if any
    // of that drifted the second request would queue behind its own predecessor —
    // appearing as playback that stalls for no reason.
    int status = 0;
    QVERIFY(hold("a.mp4", m_family[0], &status) != nullptr);
    QCOMPARE(status, 200);
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 1; }));

    // Fill the rest of the pool, so a seek that failed to reuse the slot would have
    // to queue and then be refused rather than quietly succeeding.
    int filler = 0;
    QVERIFY(hold("b.mp4", m_family[1], &filler) != nullptr);
    QCOMPARE(filler, 200);
    QVERIFY(hold("c.mp4", m_family[2], &filler) != nullptr);
    QCOMPARE(filler, 200);
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 3; }));

    auto* seek = new QTcpSocket;
    seek->setReadBufferSize(ClientReadBufferBytes);
    seek->connectToHost(QHostAddress::LocalHost, m_port);
    QVERIFY(spinUntil([seek] {
        return seek->state() == QAbstractSocket::ConnectedState;
    }, 5000));
    m_open.append(seek);

    seek->write("GET /a.mp4 HTTP/1.1\r\nHost: nas\r\n"
                "Cookie: nd_session=" + m_family[0] + "\r\n"
                "Range: bytes=3000000-3000100\r\n\r\n");
    seek->flush();

    int seekStatus = 0;
    QByteArray head;
    QVERIFY2(readStatus(seek, &seekStatus, &head, 8000),
             "a seek on an already-streaming file got no response at all");
    QVERIFY2(seekStatus == 206,
             qPrintable(QStringLiteral("expected 206 for the seek, got %1; the session "
                                       "identity is not stable across connections")
                            .arg(seekStatus)));
}

void TestAdmissionAuth::anAnonymousTransferNeverReachesAdmission()
{
    // Why the identity can be absent at all, and what happens then. With the gate
    // installed the answer is that it never gets that far — so the fallback in
    // Connection::streamClient() is unreachable today, and this test says so rather
    // than pretending to exercise it. What it does rule out is a refusal that leaks a
    // slot on the way out, which would be invisible until the pool ran dry.
    int status = 0;
    QVERIFY(hold("a.mp4", {}, &status) != nullptr);

    QVERIFY2(status == 302 || status == 401,
             qPrintable(QStringLiteral("an anonymous transfer was answered %1")
                            .arg(status)));
    QCOMPARE(m_server->activeStreams(), 0);
}

QTEST_GUILESS_MAIN(TestAdmissionAuth)
#include "tst_admissionauth.moc"
