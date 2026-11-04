#include <QTest>

#include <QDir>

#include "http/HardwareTier.h"

using http::HardwareProfile;
using http::Tier;

/**
 * Tests for http::detectHardwareProfile.
 *
 * Detection reads the real machine, so the values cannot be asserted exactly.
 * What can be asserted are the invariants every tier must satisfy, and that the
 * derived limits are internally consistent — a profile claiming eight stream
 * slots on one worker thread would be a bug regardless of the hardware.
 */
class TestHardwareTier : public QObject
{
    Q_OBJECT

private slots:
    void detectionProducesUsableLimits();
    void detectionReportsWhatItFound();
    void tiersAreOrdered();
    void overrideChangesOnlyStreamSlots();
    void overrideIsClamped();
    void tierNamesAreDistinct();
    void descriptionMentionsTheKeyNumbers();
    void diskQueryDoesNotCrashOnBadPath();
};

// ─────────────────────────────────────────────────────────────────────────────

void TestHardwareTier::detectionProducesUsableLimits()
{
    const HardwareProfile profile = http::detectHardwareProfile(QDir::tempPath());

    // Two workers minimum: one would let a single stalled transfer block
    // accepting work for everyone else on that thread.
    QVERIFY2(profile.workerThreads >= 2, "At least two worker threads are required");
    QVERIFY(profile.workerThreads <= 16);

    QVERIFY(profile.thumbnailThreads >= 1);
    QVERIFY2(profile.thumbnailThreads <= profile.workerThreads,
             "Thumbnail generation must not outnumber the threads serving requests");

    QVERIFY2(profile.streamSlots >= 2,
             "A single stream slot would stop two people using the server at once");

    // Large enough to amortise seek cost, small enough that the worst case across
    // every slot stays modest: the whole point of pairing chunk size with the cap.
    QVERIFY(profile.readChunkSize >= 64 * 1024);
    QVERIFY(profile.readChunkSize <= 4 * 1024 * 1024);

    const qint64 worstCaseBuffers = profile.readChunkSize * profile.streamSlots;
    QVERIFY2(worstCaseBuffers <= 16 * 1024 * 1024,
             qPrintable(QStringLiteral("Worst-case read buffers total %1 MB")
                            .arg(worstCaseBuffers / (1024 * 1024))));
}

void TestHardwareTier::detectionReportsWhatItFound()
{
    const HardwareProfile profile = http::detectHardwareProfile(QDir::tempPath());

    // Shown in Settings so a user can judge whether to override. A zero here
    // would mean detection silently failed.
    QVERIFY2(profile.logicalCores >= 1, "Core count was not detected");
    QVERIFY2(profile.totalMemoryBytes > 0, "Physical memory was not detected");
}

void TestHardwareTier::tiersAreOrdered()
{
    // Whatever this machine is, the tier it lands in must be consistent with the
    // limits handed out: Light is the conservative end in every dimension.
    const HardwareProfile profile = http::detectHardwareProfile();

    if (profile.tier == Tier::Light) {
        QCOMPARE(profile.streamSlots, 2);
        QCOMPARE(profile.workerThreads, 2);
    } else {
        QVERIFY(profile.streamSlots > 2);
        QVERIFY(profile.workerThreads > 2);
    }
}

void TestHardwareTier::overrideChangesOnlyStreamSlots()
{
    const HardwareProfile detected = http::detectHardwareProfile();
    const HardwareProfile override = http::profileWithStreamSlots(detected, 1);

    QCOMPARE(override.streamSlots, 1);

    // Thread counts and chunk size describe the machine, not the user's
    // preference about sharing, so an override must leave them alone.
    QCOMPARE(override.workerThreads,    detected.workerThreads);
    QCOMPARE(override.thumbnailThreads, detected.thumbnailThreads);
    QCOMPARE(override.readChunkSize,    detected.readChunkSize);
    QCOMPARE(override.tier,             detected.tier);
}

void TestHardwareTier::overrideIsClamped()
{
    const HardwareProfile detected = http::detectHardwareProfile();

    QCOMPARE(http::profileWithStreamSlots(detected, 0).streamSlots, 1);
    QCOMPARE(http::profileWithStreamSlots(detected, -5).streamSlots, 1);
    QCOMPARE(http::profileWithStreamSlots(detected, 10'000).streamSlots, 64);
}

void TestHardwareTier::tierNamesAreDistinct()
{
    const QString light    = http::tierName(Tier::Light);
    const QString balanced = http::tierName(Tier::Balanced);
    const QString capable  = http::tierName(Tier::Capable);

    QVERIFY(!light.isEmpty() && !balanced.isEmpty() && !capable.isEmpty());
    QVERIFY(light != balanced);
    QVERIFY(balanced != capable);
    QVERIFY(light != capable);
}

void TestHardwareTier::descriptionMentionsTheKeyNumbers()
{
    const HardwareProfile profile = http::detectHardwareProfile(QDir::tempPath());
    const QString text = http::describeProfile(profile);

    QVERIFY(text.contains(http::tierName(profile.tier)));
    QVERIFY(text.contains(QString::number(profile.logicalCores)));
    QVERIFY(text.contains(QString::number(profile.streamSlots)));
}

void TestHardwareTier::diskQueryDoesNotCrashOnBadPath()
{
    // Detection runs at startup against a configured path that may not exist, so
    // it must degrade to Unknown rather than fail.
    const HardwareProfile missing =
        http::detectHardwareProfile(QStringLiteral("Z:/no/such/place"));
    QVERIFY(missing.workerThreads >= 2);

    const HardwareProfile empty = http::detectHardwareProfile(QString());
    QCOMPARE(empty.disk, HardwareProfile::DiskKind::Unknown);
}

QTEST_APPLESS_MAIN(TestHardwareTier)
#include "tst_hardwaretier.moc"
