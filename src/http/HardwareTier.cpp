#include "http/HardwareTier.h"

#include <QFileInfo>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winioctl.h>
#elif defined(Q_OS_MACOS)
#  include <sys/sysctl.h>
#  include <sys/types.h>
#else
#  include <QFile>
#  include <unistd.h>
#endif

namespace {

/** Installed physical memory in bytes, or 0 when it cannot be determined. */
qint64 totalPhysicalMemory()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<qint64>(status.ullTotalPhys);
    return 0;

#elif defined(Q_OS_MACOS)
    qint64 bytes = 0;
    size_t size  = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0)
        return bytes;
    return 0;

#else
    const long pages    = ::sysconf(_SC_PHYS_PAGES);
    const long pageSize = ::sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0)
        return static_cast<qint64>(pages) * static_cast<qint64>(pageSize);
    return 0;
#endif
}

#ifdef Q_OS_WIN
/**
 * Asks the storage driver whether the volume holding @p path has a seek penalty.
 *
 * Opened with no access rights at all, which is enough for this query and avoids
 * needing administrator privileges — requesting GENERIC_READ on a volume handle
 * would.
 */
http::HardwareProfile::DiskKind queryDiskKind(const QString& path)
{
    using DiskKind = http::HardwareProfile::DiskKind;

    wchar_t volumePath[MAX_PATH] = {};
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!GetVolumePathNameW(reinterpret_cast<const wchar_t*>(absolute.utf16()),
                            volumePath, MAX_PATH)) {
        return DiskKind::Unknown;
    }

    // "C:\" must become "\\.\C:" to open the volume itself.
    QString volume = QString::fromWCharArray(volumePath);
    while (volume.endsWith(QLatin1Char('\\')))
        volume.chop(1);
    const QString devicePath = QStringLiteral("\\\\.\\") + volume;

    const HANDLE handle = CreateFileW(
        reinterpret_cast<const wchar_t*>(devicePath.utf16()),
        0,                                              // No access: query only.
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        return DiskKind::Unknown;

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType  = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;

    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
                                    &query, sizeof(query),
                                    &descriptor, sizeof(descriptor),
                                    &returned, nullptr);
    CloseHandle(handle);

    if (!ok || returned < sizeof(descriptor))
        return DiskKind::Unknown;

    return descriptor.IncursSeekPenalty ? DiskKind::Rotational : DiskKind::SolidState;
}

#elif defined(Q_OS_LINUX)
/** Reads /sys/block/<device>/queue/rotational for the volume holding @p path. */
http::HardwareProfile::DiskKind queryDiskKind(const QString& path)
{
    using DiskKind = http::HardwareProfile::DiskKind;
    Q_UNUSED(path)

    // Mapping a path to its backing block device means walking /proc/mounts and
    // then resolving device-mapper and partition layers. That is a meaningful
    // amount of code for a signal that only nudges the tier, so it is left for
    // when there is a Linux user to test it against. Cores and memory still
    // classify the machine.
    return DiskKind::Unknown;
}

#else
http::HardwareProfile::DiskKind queryDiskKind(const QString& path)
{
    Q_UNUSED(path)
    // macOS exposes this through IOKit rather than a simple query, and Apple
    // hardware of the supported era is solid state regardless.
    return http::HardwareProfile::DiskKind::Unknown;
}
#endif

/** Applies the limits that go with @p tier. */
void applyTierLimits(http::HardwareProfile& profile, http::Tier tier)
{
    using http::Tier;

    profile.tier = tier;

    switch (tier) {
    case Tier::Light:
        profile.workerThreads    = 2;
        profile.thumbnailThreads = 1;
        profile.streamSlots      = 2;
        // Larger chunks, specifically because concurrency is bounded. On a
        // 5400 rpm disk several interleaved streams at 256 KB make the head seek
        // constantly; at most two streams of 1 MB stays near-sequential, which
        // amortises seek cost several times over.
        profile.readChunkSize    = 1024 * 1024;
        break;

    case Tier::Balanced:
        profile.workerThreads    = 3;
        profile.thumbnailThreads = 2;
        profile.streamSlots      = 4;
        profile.readChunkSize    = 512 * 1024;
        break;

    case Tier::Capable:
        profile.workerThreads    = 4;
        profile.thumbnailThreads = 2;
        profile.streamSlots      = 8;
        profile.readChunkSize    = 512 * 1024;
        break;
    }
}

} // namespace

namespace http {

HardwareProfile detectHardwareProfile(const QString& pathOnServedVolume)
{
    HardwareProfile profile;

    profile.logicalCores     = std::max(1, QThread::idealThreadCount());
    profile.totalMemoryBytes = totalPhysicalMemory();

    if (!pathOnServedVolume.isEmpty())
        profile.disk = queryDiskKind(pathOnServedVolume);

    constexpr qint64 GiB = 1024LL * 1024 * 1024;

    // Any one weak dimension puts the machine in Light. A fast CPU does not help
    // when the disk thrashes, and plenty of RAM does not help on two cores.
    const bool fewCores     = profile.logicalCores <= 2;
    const bool littleMemory = profile.totalMemoryBytes > 0
                              && profile.totalMemoryBytes <= 4 * GiB;
    const bool spinningDisk = profile.disk == HardwareProfile::DiskKind::Rotational;

    if (fewCores || littleMemory || spinningDisk) {
        applyTierLimits(profile, Tier::Light);
    } else if (profile.logicalCores >= 8 && profile.totalMemoryBytes >= 16 * GiB) {
        applyTierLimits(profile, Tier::Capable);
    } else {
        applyTierLimits(profile, Tier::Balanced);
    }

    return profile;
}

HardwareProfile profileWithStreamSlots(const HardwareProfile& detected, int streamSlots)
{
    HardwareProfile profile = detected;
    profile.streamSlots = std::clamp(streamSlots, 1, 64);
    return profile;
}

QString tierName(Tier tier)
{
    switch (tier) {
    case Tier::Light:    return QStringLiteral("Light");
    case Tier::Balanced: return QStringLiteral("Balanced");
    case Tier::Capable:  return QStringLiteral("Capable");
    }
    return QStringLiteral("Unknown");
}

QString describeProfile(const HardwareProfile& profile)
{
    QString diskText;
    switch (profile.disk) {
    case HardwareProfile::DiskKind::Rotational: diskText = QStringLiteral("HDD");     break;
    case HardwareProfile::DiskKind::SolidState: diskText = QStringLiteral("SSD");     break;
    case HardwareProfile::DiskKind::Unknown:    diskText = QStringLiteral("unknown"); break;
    }

    const double memoryGiB = profile.totalMemoryBytes
                                 ? profile.totalMemoryBytes / double(1024LL * 1024 * 1024)
                                 : 0.0;

    return QStringLiteral("%1 tier — %2 cores, %3 GB RAM, disk %4 → "
                          "%5 workers, %6 stream slot(s), %7 KB chunks")
        .arg(tierName(profile.tier))
        .arg(profile.logicalCores)
        .arg(memoryGiB, 0, 'f', 1)
        .arg(diskText)
        .arg(profile.workerThreads)
        .arg(profile.streamSlots)
        .arg(profile.readChunkSize / 1024);
}

} // namespace http
