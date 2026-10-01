#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "handlers/FileRouter.h"
#include "http/AdmissionControl.h"
#include "http/HardwareTier.h"
#include "http/HttpServer.h"

namespace {

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

/** Reads only the status line and headers, deliberately leaving the body unread. */
bool readHeadersOnly(QTcpSocket* socket, int* status, QByteArray* head,
                     int timeoutMs = 5000)
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
 * End-to-end admission control.
 *
 * The unit tests in tst_admissioncontrol cover the bookkeeping. These drive real
 * sockets, because the behaviour that matters is whether a slot survives the
 * things a network actually does: a client that stops reading, and a client that
 * disappears mid-transfer.
 *
 * A single slot is configured so capacity is reached deterministically rather
 * than by timing.
 *
 * Note that ClientId is the peer address until sessions arrive in step 15, so
 * every connection from localhost counts as one client here. That is also true of
 * real clients sharing a household NAT, which is one reason the per-client limit
 * exists rather than relying on the global ceiling alone.
 */
class TestAdmissionServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void smallFilesBypassWhileThePoolIsFull();
    void secondLargeTransferIsRefusedWithRetryAfter();
    void seekOnTheSameFileIsNotRefused();
    void slotIsReclaimedAfterACompletedTransfer();
    void slotIsReclaimedWhenAClientVanishesMidTransfer();
    void stalledClientIsAbandonedAndItsSlotReclaimed();

private:
    QTcpSocket* connect_();

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<handlers::FileRouter> m_router;
    std::unique_ptr<http::HttpServer>     m_server;
    quint16                               m_port = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionServer::initTestCase()
{
    QVERIFY2(m_tmp.isValid(), qPrintable(m_tmp.errorString()));
    m_root = m_tmp.path();

    // Comfortably over the 2 MB threshold, and large enough that it cannot be
    // written into the socket buffers in one go — the transfer has to still be in
    // progress while the test inspects the slot.
    QByteArray payload(12 * 1024 * 1024, 'x');
    for (const QString& name : { QStringLiteral("big-a.mp4"), QStringLiteral("big-b.mp4") }) {
        QFile file(m_root + QLatin1Char('/') + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(payload), payload.size());
        file.close();
    }

    QFile thumb(m_root + QStringLiteral("/thumb.jpg"));
    QVERIFY(thumb.open(QIODevice::WriteOnly));
    thumb.write(QByteArray(32 * 1024, 'j'));
    thumb.close();

    m_router = std::make_unique<handlers::FileRouter>(m_root);

    // Admission control, not authorisation. The router denies anonymous requests
    // unless told otherwise.
    m_router->setAnonymousAccess(true);
    m_server = std::make_unique<http::HttpServer>(m_router.get());

    // Exactly one shared slot, and no admin reserve in play since roles do not
    // exist yet.
    http::HardwareProfile profile =
        http::profileWithStreamSlots(http::detectHardwareProfile(), 1);
    m_server->setHardwareProfile(profile);

    // Short waits so the refusal path completes quickly.
    http::ConnectionLimits limits = m_server->connectionLimits();
    limits.admissionWaitMs  = 700;
    limits.admissionRetryMs = 50;
    limits.stallTimeoutMs   = 1500;
    m_server->setConnectionLimits(limits);

    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();
    QVERIFY(m_port != 0);
}

void TestAdmissionServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_router.reset();
}

QTcpSocket* TestAdmissionServer::connect_()
{
    auto* socket = new QTcpSocket(this);
    socket->connectToHost(QHostAddress::LocalHost, m_port);
    if (!spinUntil([socket] { return socket->state() == QAbstractSocket::ConnectedState; })) {
        delete socket;
        return nullptr;
    }
    return socket;
}

// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionServer::smallFilesBypassWhileThePoolIsFull()
{
    QTcpSocket* holder = connect_();
    QVERIFY(holder);
    holder->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");
    holder->flush();

    int status = 0;
    QByteArray head;
    QVERIFY(readHeadersOnly(holder, &status, &head));
    QCOMPARE(status, 200);
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 1; }));

    // The point of the size threshold: browsing stays responsive while someone
    // else is downloading a film. If thumbnails needed slots, the grid would
    // stall behind a single transfer.
    QTcpSocket* browser = connect_();
    QVERIFY(browser);
    browser->write("GET /thumb.jpg HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    browser->flush();

    int thumbStatus = 0;
    QByteArray thumbHead;
    QVERIFY2(readHeadersOnly(browser, &thumbStatus, &thumbHead),
             "A small file was not served while the pool was full");
    QCOMPARE(thumbStatus, 200);

    holder->abort();
    browser->abort();
    holder->deleteLater();
    browser->deleteLater();
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 0; }));
}

void TestAdmissionServer::secondLargeTransferIsRefusedWithRetryAfter()
{
    QTcpSocket* holder = connect_();
    QVERIFY(holder);
    holder->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");
    holder->flush();

    int status = 0;
    QByteArray head;
    QVERIFY(readHeadersOnly(holder, &status, &head));
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 1; }));

    // A different large file, so no slot can be reused. Expect a wait followed by
    // an honest refusal rather than an indefinite hang.
    QTcpSocket* second = connect_();
    QVERIFY(second);
    second->write("GET /big-b.mp4 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    second->flush();

    int refusedStatus = 0;
    QByteArray refusedHead;
    QVERIFY(readHeadersOnly(second, &refusedStatus, &refusedHead, 8000));
    QCOMPARE(refusedStatus, 503);
    QVERIFY2(refusedHead.contains("Retry-After:"),
             "A 503 from admission control must tell the client when to retry");

    holder->abort();
    second->abort();
    holder->deleteLater();
    second->deleteLater();
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 0; }));
}

void TestAdmissionServer::seekOnTheSameFileIsNotRefused()
{
    // The deadlock this design exists to avoid. A player abandons its connection
    // and immediately reopens with a Range header; if that counted as a new
    // stream it would queue behind the slot its own predecessor still holds, and
    // playback would stall for no visible reason.
    QTcpSocket* first = connect_();
    QVERIFY(first);
    first->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");
    first->flush();

    int status = 0;
    QByteArray head;
    QVERIFY(readHeadersOnly(first, &status, &head));
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 1; }));

    QTcpSocket* seek = connect_();
    QVERIFY(seek);
    seek->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\n"
                "Range: bytes=6000000-6000100\r\nConnection: close\r\n\r\n");
    seek->flush();

    int seekStatus = 0;
    QByteArray seekHead;
    QVERIFY2(readHeadersOnly(seek, &seekStatus, &seekHead, 3000),
             "A seek on an already-streaming file got no response");
    QVERIFY2(seekStatus == 206,
             qPrintable(QStringLiteral("Expected 206 for the seek, got %1").arg(seekStatus)));

    first->abort();
    seek->abort();
    first->deleteLater();
    seek->deleteLater();
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 0; }));
}

void TestAdmissionServer::slotIsReclaimedAfterACompletedTransfer()
{
    for (int round = 0; round < 3; ++round) {
        QTcpSocket* socket = connect_();
        QVERIFY(socket);
        socket->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        socket->flush();

        // Drain fully, so the transfer finishes rather than being abandoned.
        qint64 received = 0;
        const bool drained = spinUntil([&] {
            received += socket->readAll().size();
            return socket->state() != QAbstractSocket::ConnectedState;
        }, 20'000);

        QVERIFY2(drained, qPrintable(QStringLiteral("round %1 did not finish").arg(round)));
        QVERIFY(received > 12 * 1024 * 1024);

        socket->deleteLater();
        QVERIFY2(spinUntil([this] { return m_server->activeStreams() == 0; }),
                 qPrintable(QStringLiteral("slot not released after round %1").arg(round)));
    }
}

void TestAdmissionServer::slotIsReclaimedWhenAClientVanishesMidTransfer()
{
    // The failure that would be hardest to attribute in the field: a device
    // leaving coverage mid-download. Repeat it more times than there are slots —
    // if any slot leaked, a later round would be refused and the server would
    // look like it had hung.
    for (int round = 0; round < 6; ++round) {
        QTcpSocket* socket = connect_();
        QVERIFY(socket);
        socket->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");
        socket->flush();

        int status = 0;
        QByteArray head;
        QVERIFY2(readHeadersOnly(socket, &status, &head, 8000),
                 qPrintable(QStringLiteral("round %1 was never answered — a slot leaked")
                                .arg(round)));
        QCOMPARE(status, 200);

        // abort() rather than disconnectFromHost(): no graceful shutdown, which is
        // what a vanished client looks like.
        socket->abort();
        socket->deleteLater();

        QVERIFY2(spinUntil([this] { return m_server->activeStreams() == 0; }, 8000),
                 qPrintable(QStringLiteral("slot not reclaimed after abort in round %1")
                                .arg(round)));
    }
}

void TestAdmissionServer::stalledClientIsAbandonedAndItsSlotReclaimed()
{
    // Distinct from an abort, and nastier. The client stops reading but keeps the
    // socket open, which is what a phone leaving coverage looks like: no FIN ever
    // arrives, so the connection stays nominally alive indefinitely. The idle
    // timer cannot catch this — a stalled transfer looks busy, not idle — so
    // without the separate stall timer the slot would be held for good.
    QTcpSocket* stalled = connect_();
    QVERIFY(stalled);
    stalled->write("GET /big-a.mp4 HTTP/1.1\r\nHost: h\r\n\r\n");
    stalled->flush();

    int status = 0;
    QByteArray head;
    QVERIFY(readHeadersOnly(stalled, &status, &head));
    QCOMPARE(status, 200);
    QVERIFY(spinUntil([this] { return m_server->activeStreams() == 1; }));

    // From here the socket is never read again. Writes back up, bytesWritten stops
    // firing, and the stall timer is the only thing that can notice.
    QElapsedTimer timer;
    timer.start();

    const bool reclaimed = spinUntil([this] { return m_server->activeStreams() == 0; },
                                     6000);

    QVERIFY2(reclaimed, "A stalled stream held its slot indefinitely");
    QVERIFY2(timer.elapsed() >= 500,
             "The slot was freed too early to have come from the stall timer; "
             "the connection probably closed for another reason");

    stalled->abort();
    stalled->deleteLater();
}

QTEST_GUILESS_MAIN(TestAdmissionServer)
#include "tst_admissionserver.moc"
