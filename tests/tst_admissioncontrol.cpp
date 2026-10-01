#include <QTest>

#include <QThread>
#include <vector>

#include "http/AdmissionControl.h"

using http::Admission;
using http::AdmissionControl;
using http::AdmissionResult;
using http::ClientId;
using http::StreamSlot;

namespace {

constexpr qint64 Large = 50 * 1024 * 1024;   // Needs a slot.
constexpr qint64 Small = 100 * 1024;         // Below the threshold.

} // namespace

/**
 * Tests for http::AdmissionControl.
 *
 * The release cases come first and matter most. A leaked slot does not present as
 * a crash or an error: the server simply stops serving large files once the pool
 * is exhausted, and looks like it has hung. That is far harder to diagnose later
 * than it is to test now.
 */
class TestAdmissionControl : public QObject
{
    Q_OBJECT

private slots:
    // ── Release behaviour: the leak cases ─────────────────────────────────
    void slotIsReleasedOnDestruction();
    void slotIsReleasedOnAbandonedTransfer();
    void explicitReleaseIsIdempotent();
    void movedSlotReleasesExactlyOnce();
    void moveAssignmentReleasesTheOverwrittenSlot();

    // ── Admission rules ──────────────────────────────────────────────────
    void smallTransfersBypassEntirely();
    void thresholdBoundaryIsInclusive();
    void grantsUpToTheTotal();
    void refusesBeyondTheTotal();
    void capacityRecoversAfterRelease();
    void perClientLimitIsEnforced();
    void oneClientCannotStarveAnother();

    // ── The seek case ────────────────────────────────────────────────────
    void sameFileReusesTheSameSlot();
    void reuseSurvivesTheOldHolderClosing();
    void differentFilesTakeDifferentSlots();

    // ── Administrator reserve ────────────────────────────────────────────
    void adminGetsTheReservedSlot();
    void adminReserveIsNotAvailableToOthers();

    // ── Bookkeeping ──────────────────────────────────────────────────────
    void countsDoNotDriftOverManyCycles();
    void concurrentRequestsNeverExceedTheTotal();
};

// ─────────────────────────────────────────────────────────────────────────────
//  Release behaviour
// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionControl::slotIsReleasedOnDestruction()
{
    AdmissionControl control(2);

    {
        AdmissionResult granted = control.request("ip-a", "/movie.mp4", Large);
        QCOMPARE(granted.outcome, Admission::Granted);
        QCOMPARE(control.activeStreams(), 1);
    }

    QCOMPARE(control.activeStreams(), 0);
}

void TestAdmissionControl::slotIsReleasedOnAbandonedTransfer()
{
    // Simulates the failure that matters most in the field: a phone drops off
    // mobile data mid-download, so the transfer never completes and the owning
    // object is destroyed without anything resembling a clean finish. If the slot
    // survived that, a handful of such downloads would wedge the server.
    AdmissionControl control(2);

    for (int attempt = 0; attempt < 10; ++attempt) {
        AdmissionResult granted =
            control.request("ip-a", QStringLiteral("/clip-%1.mp4").arg(attempt), Large);
        QVERIFY2(granted.outcome == Admission::Granted,
                 qPrintable(QStringLiteral("attempt %1 was refused — slots leaked")
                                .arg(attempt)));
        // Scope ends here with no explicit release, exactly as an aborted
        // transfer would.
    }

    QCOMPARE(control.activeStreams(), 0);
}

void TestAdmissionControl::explicitReleaseIsIdempotent()
{
    AdmissionControl control(2);

    AdmissionResult granted = control.request("ip-a", "/movie.mp4", Large);
    QCOMPARE(control.activeStreams(), 1);

    granted.slot.release();
    QCOMPARE(control.activeStreams(), 0);

    // A second release, and then the destructor, must not drive the count
    // negative or free a slot belonging to someone else.
    granted.slot.release();
    QCOMPARE(control.activeStreams(), 0);
    QVERIFY(!granted.slot.isHeld());
}

void TestAdmissionControl::movedSlotReleasesExactlyOnce()
{
    AdmissionControl control(1);

    {
        AdmissionResult granted = control.request("ip-a", "/movie.mp4", Large);
        QCOMPARE(granted.outcome, Admission::Granted);

        StreamSlot moved = std::move(granted.slot);
        QVERIFY(moved.isHeld());
        QVERIFY2(!granted.slot.isHeld(), "The moved-from slot must no longer hold");

        // Both go out of scope here. A double release would show up as the next
        // request succeeding when it should not.
        QCOMPARE(control.activeStreams(), 1);
    }

    QCOMPARE(control.activeStreams(), 0);

    AdmissionResult again = control.request("ip-b", "/other.mp4", Large);
    QCOMPARE(again.outcome, Admission::Granted);
    QCOMPARE(control.activeStreams(), 1);
}

void TestAdmissionControl::moveAssignmentReleasesTheOverwrittenSlot()
{
    AdmissionControl control(2);

    AdmissionResult first  = control.request("ip-a", "/one.mp4", Large);
    AdmissionResult second = control.request("ip-b", "/two.mp4", Large);
    QCOMPARE(control.activeStreams(), 2);

    // Overwriting must free what the target held, or that slot is lost for good.
    first.slot = std::move(second.slot);
    QCOMPARE(control.activeStreams(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Admission rules
// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionControl::smallTransfersBypassEntirely()
{
    AdmissionControl control(2);

    // A folder of thumbnails must remain browsable while the pool is full.
    AdmissionResult a = control.request("ip-a", "/big-1.mp4", Large);
    AdmissionResult b = control.request("ip-b", "/big-2.mp4", Large);
    QCOMPARE(control.activeStreams(), 2);

    for (int i = 0; i < 50; ++i) {
        AdmissionResult thumb =
            control.request("ip-c", QStringLiteral("/thumb-%1.jpg").arg(i), Small);
        QCOMPARE(thumb.outcome, Admission::Bypassed);
        QVERIFY2(!thumb.slot.isHeld(), "A bypass must not hold a slot");
    }

    QCOMPARE(control.activeStreams(), 2);
}

void TestAdmissionControl::thresholdBoundaryIsInclusive()
{
    AdmissionControl control(4);
    const qint64 threshold = control.slotThresholdBytes();

    AdmissionResult below = control.request("ip-a", "/a", threshold - 1);
    QCOMPARE(below.outcome, Admission::Bypassed);

    AdmissionResult atExactly = control.request("ip-a", "/b", threshold);
    QCOMPARE(atExactly.outcome, Admission::Granted);
}

void TestAdmissionControl::grantsUpToTheTotal()
{
    AdmissionControl control(3, /*perClientLimit=*/1);

    std::vector<AdmissionResult> held;
    for (int i = 0; i < 3; ++i) {
        held.push_back(control.request(QByteArray("ip-") + QByteArray::number(i),
                                       "/movie.mp4", Large));
        QCOMPARE(held.back().outcome, Admission::Granted);
    }

    QCOMPARE(control.activeStreams(), 3);
}

void TestAdmissionControl::refusesBeyondTheTotal()
{
    AdmissionControl control(2, /*perClientLimit=*/1, /*adminReserve=*/0);

    AdmissionResult a = control.request("ip-a", "/movie.mp4", Large);
    AdmissionResult b = control.request("ip-b", "/movie.mp4", Large);
    AdmissionResult c = control.request("ip-c", "/movie.mp4", Large);

    QCOMPARE(a.outcome, Admission::Granted);
    QCOMPARE(b.outcome, Admission::Granted);
    QCOMPARE(c.outcome, Admission::AtCapacity);
    QVERIFY2(!c.slot.isHeld(), "A refusal must not hold a slot");
    QCOMPARE(control.activeStreams(), 2);
}

void TestAdmissionControl::capacityRecoversAfterRelease()
{
    AdmissionControl control(1, 1, 0);

    {
        AdmissionResult a = control.request("ip-a", "/movie.mp4", Large);
        QCOMPARE(a.outcome, Admission::Granted);

        AdmissionResult b = control.request("ip-b", "/movie.mp4", Large);
        QCOMPARE(b.outcome, Admission::AtCapacity);
    }

    AdmissionResult c = control.request("ip-c", "/movie.mp4", Large);
    QCOMPARE(c.outcome, Admission::Granted);
}

void TestAdmissionControl::perClientLimitIsEnforced()
{
    AdmissionControl control(8, /*perClientLimit=*/2);

    AdmissionResult a = control.request("ip-a", "/one.mp4",   Large);
    AdmissionResult b = control.request("ip-a", "/two.mp4",   Large);
    AdmissionResult c = control.request("ip-a", "/three.mp4", Large);

    QCOMPARE(a.outcome, Admission::Granted);
    QCOMPARE(b.outcome, Admission::Granted);
    QVERIFY2(c.outcome == Admission::AtCapacity,
             "One client must not exceed its own ceiling even when slots remain");
    QCOMPARE(control.streamsFor("ip-a"), 2);
}

void TestAdmissionControl::oneClientCannotStarveAnother()
{
    // A browser fetching six files in parallel must not occupy the whole pool.
    AdmissionControl control(4, /*perClientLimit=*/2);

    std::vector<AdmissionResult> greedy;
    for (int i = 0; i < 6; ++i)
        greedy.push_back(control.request("ip-greedy",
                                        QStringLiteral("/file-%1.mp4").arg(i), Large));

    QCOMPARE(control.streamsFor("ip-greedy"), 2);

    AdmissionResult other = control.request("ip-other", "/movie.mp4", Large);
    QCOMPARE(other.outcome, Admission::Granted);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The seek case
// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionControl::sameFileReusesTheSameSlot()
{
    // The deadlock this design exists to prevent. A player closes its connection
    // and reopens with a Range header; if that second request took a new slot it
    // would queue behind the one its own predecessor still holds.
    AdmissionControl control(1, 2, 0);

    AdmissionResult first = control.request("ip-a", "/movie.mp4", Large);
    QCOMPARE(first.outcome, Admission::Granted);
    QCOMPARE(control.activeStreams(), 1);

    AdmissionResult seek = control.request("ip-a", "/movie.mp4", Large);
    QVERIFY2(seek.outcome == Admission::Granted,
             "A seek on an already-streaming file must not be refused");
    QCOMPARE(control.activeStreams(), 1);
}

void TestAdmissionControl::reuseSurvivesTheOldHolderClosing()
{
    // Release order is not guaranteed: the old connection may close before or
    // after the new one is admitted. Reference counting makes both orders safe.
    AdmissionControl control(1, 2, 0);

    AdmissionResult first = control.request("ip-a", "/movie.mp4", Large);
    AdmissionResult seek  = control.request("ip-a", "/movie.mp4", Large);
    QCOMPARE(control.activeStreams(), 1);

    first.slot.release();                       // Old connection goes away.
    QVERIFY2(control.activeStreams() == 1,
             "The surviving holder must keep the slot alive");

    seek.slot.release();
    QCOMPARE(control.activeStreams(), 0);
}

void TestAdmissionControl::differentFilesTakeDifferentSlots()
{
    AdmissionControl control(4);

    AdmissionResult a = control.request("ip-a", "/one.mp4", Large);
    AdmissionResult b = control.request("ip-a", "/two.mp4", Large);

    QCOMPARE(control.activeStreams(), 2);
    QCOMPARE(control.streamsFor("ip-a"), 2);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Administrator reserve
// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionControl::adminGetsTheReservedSlot()
{
    // The owner of the machine must not be locked out of their own storage.
    AdmissionControl control(2, 2, /*adminReserve=*/1);

    AdmissionResult a = control.request("ip-a", "/one.mp4", Large);
    AdmissionResult b = control.request("ip-b", "/two.mp4", Large);
    QCOMPARE(control.activeStreams(), 2);

    AdmissionResult admin = control.request("ip-admin", "/three.mp4", Large, /*isAdmin=*/true);
    QCOMPARE(admin.outcome, Admission::Granted);
    QCOMPARE(control.activeStreams(), 3);
}

void TestAdmissionControl::adminReserveIsNotAvailableToOthers()
{
    AdmissionControl control(2, 2, /*adminReserve=*/1);

    AdmissionResult a = control.request("ip-a", "/one.mp4", Large);
    AdmissionResult b = control.request("ip-b", "/two.mp4", Large);

    AdmissionResult guest = control.request("ip-c", "/three.mp4", Large, /*isAdmin=*/false);
    QCOMPARE(guest.outcome, Admission::AtCapacity);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Bookkeeping
// ─────────────────────────────────────────────────────────────────────────────

void TestAdmissionControl::countsDoNotDriftOverManyCycles()
{
    // Guards against an empty per-client bucket being left behind, which would
    // make the counts creep upward over a long uptime and slowly shrink capacity.
    AdmissionControl control(2);

    for (int i = 0; i < 500; ++i) {
        AdmissionResult a = control.request(QByteArray("ip-") + QByteArray::number(i % 7),
                                            QStringLiteral("/f-%1.mp4").arg(i), Large);
        QCOMPARE(a.outcome, Admission::Granted);
    }

    QCOMPARE(control.activeStreams(), 0);
    for (int i = 0; i < 7; ++i)
        QCOMPARE(control.streamsFor(QByteArray("ip-") + QByteArray::number(i)), 0);
}

void TestAdmissionControl::concurrentRequestsNeverExceedTheTotal()
{
    // Every worker thread shares one instance, so the ceiling has to hold under
    // simultaneous requests rather than only sequential ones.
    constexpr int TotalSlots = 3;
    AdmissionControl control(TotalSlots, /*perClientLimit=*/1, /*adminReserve=*/0);

    QAtomicInt granted{0};
    QAtomicInt peak{0};

    QList<QThread*> threads;
    for (int t = 0; t < 8; ++t) {
        QThread* thread = QThread::create([&control, &granted, &peak, t] {
            for (int i = 0; i < 200; ++i) {
                AdmissionResult r = control.request(
                    QByteArray("ip-") + QByteArray::number(t), "/movie.mp4", Large);
                if (r.outcome != Admission::Granted)
                    continue;

                granted.fetchAndAddOrdered(1);

                const int active = control.activeStreams();
                int observed = peak.loadRelaxed();
                while (active > observed
                       && !peak.testAndSetOrdered(observed, active)) {
                    observed = peak.loadRelaxed();
                }
                // Slot released as r goes out of scope.
            }
        });
        threads.append(thread);
        thread->start();
    }

    for (QThread* thread : threads) {
        QVERIFY(thread->wait(30'000));
        delete thread;
    }

    QVERIFY2(granted.loadRelaxed() > 0, "No request was granted; the test proved nothing");
    QVERIFY2(peak.loadRelaxed() <= TotalSlots,
             qPrintable(QStringLiteral("Observed %1 concurrent streams with a ceiling of %2")
                            .arg(peak.loadRelaxed()).arg(TotalSlots)));
    QCOMPARE(control.activeStreams(), 0);
}

QTEST_APPLESS_MAIN(TestAdmissionControl)
#include "tst_admissioncontrol.moc"
