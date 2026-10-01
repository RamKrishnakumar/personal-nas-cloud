#include "media/ThumbnailScanner.h"

#include "db/Database.h"
#include "media/ThumbnailCache.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSqlQuery>
#include <QThread>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(Q_OS_LINUX)
#  include <QFile>
#endif

namespace {

/** Delay between files while working. Leaves headroom rather than saturating a core. */
constexpr int WorkIntervalMs = 40;

/** Delay before re-checking when yielding to a transfer. */
constexpr int YieldIntervalMs = 2'000;

/**
 * Stop scanning when the cache is this close to full.
 *
 * Without it a library larger than the cache ceiling would scan, evict its own
 * earliest results, and scan those again forever — burning CPU and disk
 * indefinitely while never converging. Demand-driven generation still works
 * normally; only the warm-up stops.
 */
constexpr double CacheFullRatio = 0.95;

/** Backstop against an unterminated walk. No real library comes close. */
constexpr int MaximumWalkDepth = 64;

/**
 * @brief True when the machine is running on battery.
 *
 * Best effort. An unknown answer is treated as mains power, so an undetectable
 * platform scans rather than never warming the cache at all.
 */
/**
 * @brief Key used to compare directories for "already visited".
 *
 * Case-folded where the filesystem is case-insensitive, so the same directory
 * reached with different capitalisation is recognised as the same place.
 */
QString normalisedKey(const QString& canonicalPath)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return canonicalPath.toCaseFolded();
#else
    return canonicalPath;
#endif
}

/**
 * @brief True when @p path is @p root or lies beneath it.
 *
 * Deliberately a small local copy rather than a dependency on http::PathResolver:
 * this library has no business knowing about the HTTP layer. The separator is
 * appended before the prefix test for the same reason it is there — without it a
 * root of `E:/Foo` would accept the sibling `E:/Foobar`.
 */
bool isWithinDirectory(const QString& root, const QString& path)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif

    if (root.isEmpty())
        return false;
    if (path.compare(root, sensitivity) == 0)
        return true;

    QString prefix = root;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');

    return path.startsWith(prefix, sensitivity);
}

bool isOnBatteryPower()
{
#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status))
        return false;

    // 255 means unknown; 1 means plugged in.
    if (status.ACLineStatus == 1)
        return false;
    return status.ACLineStatus == 0;

#elif defined(Q_OS_LINUX)
    // Any mains supply reporting "online" counts as plugged in.
    const QDir supplies(QStringLiteral("/sys/class/power_supply"));
    for (const QString& entry : supplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile typeFile(supplies.absoluteFilePath(entry + QStringLiteral("/type")));
        if (!typeFile.open(QIODevice::ReadOnly))
            continue;
        if (typeFile.readAll().trimmed() != QByteArrayLiteral("Mains"))
            continue;

        QFile onlineFile(supplies.absoluteFilePath(entry + QStringLiteral("/online")));
        if (onlineFile.open(QIODevice::ReadOnly))
            return onlineFile.readAll().trimmed() == QByteArrayLiteral("0");
    }
    return false;

#else
    return false;
#endif
}

} // namespace

namespace media {

ThumbnailScanner::ThumbnailScanner(ThumbnailCache* cache,
                                   db::Database*   database,
                                   QObject*        parent)
    : QObject(parent)
    , m_cache(cache)
    , m_database(database)
{
    qRegisterMetaType<ThumbnailScanner::Progress>();
}

ThumbnailScanner::~ThumbnailScanner()
{
    stop();

    // The worker captures `this`, so it must have finished before the object dies.
    if (m_thread) {
        if (!m_thread->wait(10'000)) {
            qWarning() << "[Scan] Scanner thread did not stop; terminating.";
            m_thread->terminate();
            m_thread->wait();
        }
        delete m_thread;
        m_thread = nullptr;
    }
}

void ThumbnailScanner::setRoot(const QString& root)
{
    QMutexLocker locker(&m_mutex);
    m_root = root;
}

void ThumbnailScanner::setBusyPredicate(std::function<bool()> predicate)
{
    QMutexLocker locker(&m_mutex);
    m_busyPredicate = std::move(predicate);
}

void ThumbnailScanner::setPauseOnBattery(bool pause)
{
    QMutexLocker locker(&m_mutex);
    m_pauseOnBattery = pause;
}

bool ThumbnailScanner::isRunning() const
{
    QMutexLocker locker(&m_mutex);
    return m_progress.running;
}

ThumbnailScanner::Progress ThumbnailScanner::progress() const
{
    QMutexLocker locker(&m_mutex);
    return m_progress;
}

void ThumbnailScanner::start()
{
    QString root;
    {
        QMutexLocker locker(&m_mutex);
        if (m_progress.running)
            return;
        root = m_root;
    }

    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        qWarning() << "[Scan] Not a directory, nothing to warm:" << root;
        return;
    }

    // Join any previous run before starting another, so there is only ever one
    // worker touching the queues.
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }

    m_stopRequested.storeRelaxed(false);
    {
        QMutexLocker locker(&m_mutex);
        m_progress = Progress{};
        m_progress.running = true;
    }

    m_pendingDirectories.clear();
    m_pendingFiles.clear();
    m_visitedDirectories.clear();

    m_rootCanonical = QFileInfo(root).canonicalFilePath();
    if (m_rootCanonical.isEmpty()) {
        QMutexLocker locker(&m_mutex);
        m_progress.running = false;
        qWarning() << "[Scan] Root is not readable:" << root;
        return;
    }

    m_visitedDirectories.insert(normalisedKey(m_rootCanonical));
    m_pendingDirectories.enqueue(m_rootCanonical);

    // A plain loop on its own thread rather than a timer-driven state machine.
    // The earlier version moved this object into the thread it owned, which also
    // moved the QThread itself and broke timer ownership; a loop needs no event
    // loop, so none of that applies.
    m_thread = QThread::create([this] { runWalk(); });
    m_thread->setObjectName(QStringLiteral("nestdrive-scanner"));

    // Serving requests must always win; the warm-up takes only spare cycles.
    m_thread->start(QThread::LowestPriority);

    qInfo() << "[Scan] Warming thumbnail cache under" << root;
}

void ThumbnailScanner::stop()
{
    m_stopRequested.storeRelaxed(true);
}

bool ThumbnailScanner::shouldYield() const
{
    std::function<bool()> busy;
    bool pauseOnBattery = true;
    {
        QMutexLocker locker(&m_mutex);
        busy = m_busyPredicate;
        pauseOnBattery = m_pauseOnBattery;
    }

    // Checked before every file rather than once per directory: a transfer can
    // start at any moment, and the point is to get out of its way promptly.
    if (busy && busy())
        return true;

    return pauseOnBattery && isOnBatteryPower();
}

void ThumbnailScanner::runWalk()
{
    const auto finish = [this](const char* reason) {
        Progress snapshot;
        {
            QMutexLocker locker(&m_mutex);
            m_progress.running = false;
            m_progress.waiting = false;
            snapshot = m_progress;
        }
        qInfo() << "[Scan]" << reason
                << snapshot.directoriesVisited << "directories,"
                << snapshot.filesExamined << "files examined,"
                << snapshot.thumbnailsCreated << "thumbnails created.";
        emit finished();
    };

    while (!m_stopRequested.loadRelaxed()) {
        if (shouldYield()) {
            {
                QMutexLocker locker(&m_mutex);
                m_progress.waiting = true;
            }
            QThread::msleep(YieldIntervalMs);
            continue;
        }

        {
            QMutexLocker locker(&m_mutex);
            m_progress.waiting = false;
        }

        // Stop rather than scan into a full cache: a library larger than the
        // ceiling would otherwise evict its own earliest results and regenerate
        // them forever, never converging. On-demand generation is unaffected.
        if (m_cache->maxBytes() > 0
            && m_cache->totalBytes() >= qint64(m_cache->maxBytes() * CacheFullRatio)) {
            finish("Cache at capacity, warm-up stopped after");
            return;
        }

        if (!m_pendingFiles.isEmpty()) {
            const QString file = m_pendingFiles.dequeue();
            const bool created = m_cache->generateBlocking(file, ThumbTier::Small);

            Progress snapshot;
            {
                QMutexLocker locker(&m_mutex);
                ++m_progress.filesExamined;
                if (created)
                    ++m_progress.thumbnailsCreated;
                m_progress.directoriesQueued = int(m_pendingDirectories.size());
                snapshot = m_progress;
            }

            emit progressChanged(snapshot);

            // Deliberate idle between files, so the warm-up never saturates a core
            // even when the server looks idle.
            QThread::msleep(WorkIntervalMs);
            continue;
        }

        if (!m_pendingDirectories.isEmpty()) {
            const QString directory = m_pendingDirectories.dequeue();
            enqueueChildren(directory);

            QMutexLocker locker(&m_mutex);
            ++m_progress.directoriesVisited;
            m_progress.directoriesQueued = int(m_pendingDirectories.size());
            continue;
        }

        finish("Finished:");
        return;
    }

    finish("Stopped after");
}

void ThumbnailScanner::enqueueChildren(const QString& directoryPath)
{
    QDir directory(directoryPath);

    // Subdirectories are always queued, even when this one needs no sweep: a
    // parent's modification time does not change when a grandchild gains files.
    // Depth is measured from the root as a final backstop. Nothing should reach it,
    // but an unterminated walk burns CPU indefinitely, so it is cheap insurance.
    const int depth = directoryPath.count(QLatin1Char('/'))
                      - m_rootCanonical.count(QLatin1Char('/'));
    if (depth >= MaximumWalkDepth) {
        qWarning() << "[Scan] Depth limit reached, not descending further:" << directoryPath;
        return;
    }

    for (const QFileInfo& entry : directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        // Links are skipped outright. canonicalFilePath() cannot be relied on to
        // resolve them: on Windows it leaves an NTFS junction unresolved and
        // isSymLink() reports false for one, so a junction pointing at an ancestor
        // looks like an ordinary new directory and the walk recurses forever.
        //
        // Skipping loses nothing. A link inside the root points at a directory the
        // walk reaches by its real path anyway, and one pointing outside the root
        // is something PathResolver refuses to serve through — so warming it would
        // produce thumbnails for files no client can fetch.
        if (entry.isSymbolicLink())
            continue;
#ifdef Q_OS_WIN
        if (entry.isJunction())
            continue;
#endif

        const QString canonical = entry.canonicalFilePath();
        if (canonical.isEmpty())
            continue;                  // Unreadable, or a dangling link.

        if (!isWithinDirectory(m_rootCanonical, canonical))
            continue;

        const QString key = normalisedKey(canonical);
        if (m_visitedDirectories.contains(key))
            continue;

        m_visitedDirectories.insert(key);
        m_pendingDirectories.enqueue(entry.absoluteFilePath());
    }

    if (!directoryNeedsSweep(directoryPath))
        return;

    for (const QFileInfo& entry : directory.entryInfoList(QDir::Files)) {
        if (ThumbnailCache::isSupportedImage(entry.fileName()))
            m_pendingFiles.enqueue(entry.absoluteFilePath());
    }

    recordDirectorySwept(directoryPath);
}

bool ThumbnailScanner::directoryNeedsSweep(const QString& directoryPath) const
{
    if (!m_database || !m_database->isOpen())
        return true;        // No record to consult; sweep it.

    qint64 sweptAt = 0;
    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral("SELECT done_at FROM scan_state WHERE path = ?"));
        query.addBindValue(directoryPath);
        if (!query.exec() || !query.next())
            return true;
        sweptAt = query.value(0).toLongLong();
    }

    // Revisited when its contents changed since the last sweep, which is how new
    // photos get picked up without walking the whole library again.
    const qint64 modifiedAt =
        QFileInfo(directoryPath).lastModified().toSecsSinceEpoch();

    return modifiedAt > sweptAt;
}

void ThumbnailScanner::recordDirectorySwept(const QString& directoryPath)
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO scan_state (path, done_at) VALUES (?, ?)"));
    query.addBindValue(directoryPath);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

} // namespace media
