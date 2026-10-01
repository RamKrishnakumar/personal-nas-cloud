#pragma once

#include <QString>
#include <QtGlobal>

namespace http {

/**
 * @brief How much work this machine can serve at once.
 *
 * Tiers exist so the defaults suit the machine rather than the developer's. The
 * reference target is a 2015-era dual-core laptop with 4 GB of RAM and a spinning
 * disk; a modern desktop should not be throttled to the same budget, and this
 * software is meant to be installed by strangers.
 */
enum class Tier
{
    Light,      ///< Two cores or less, or 4 GB or less, or a spinning disk.
    Balanced,   ///< Four cores, 8 GB or more.
    Capable,    ///< Eight cores or more, 16 GB or more.
};

/**
 * @brief Detected hardware, and the limits derived from it.
 *
 * Detected values are kept alongside the derived ones so Settings can show what
 * was found. Every detection heuristic is wrong for somebody, and seeing the
 * inputs is what lets a user tell whether the override is worth setting.
 */
struct HardwareProfile
{
    Tier tier = Tier::Light;

    // ── Derived limits ────────────────────────────────────────────────────
    int    workerThreads    = 2;
    int    thumbnailThreads = 1;
    int    streamSlots      = 2;
    qint64 readChunkSize    = 1024 * 1024;

    // ── What detection actually found ─────────────────────────────────────
    int    logicalCores     = 0;
    qint64 totalMemoryBytes = 0;

    /**
     * Whether the disk holding the served files incurs a seek penalty.
     * Unknown on platforms where it cannot be queried cheaply, in which case it
     * does not contribute to the tier.
     */
    enum class DiskKind { Unknown, Rotational, SolidState };
    DiskKind disk = DiskKind::Unknown;
};

/**
 * @brief Detects this machine's profile.
 *
 * @param pathOnServedVolume A path on the volume that will be served, used for
 *                           the disk query. Pass an empty string to skip it.
 *
 * Detection is cheap and done once at startup.
 */
HardwareProfile detectHardwareProfile(const QString& pathOnServedVolume = QString());

/**
 * @brief Builds a profile for an explicit stream-slot count.
 *
 * Used for the Settings override. Thread and chunk sizing still come from the
 * detected tier, since those are about the machine rather than the user's
 * preference about sharing.
 */
HardwareProfile profileWithStreamSlots(const HardwareProfile& detected, int streamSlots);

QString tierName(Tier tier);

/** One-line summary for logs and the Settings dialog. */
QString describeProfile(const HardwareProfile& profile);

} // namespace http
