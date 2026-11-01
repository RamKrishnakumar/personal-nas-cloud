#pragma once

#include <QMutex>
#include <QAtomicInteger>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

#include <functional>

namespace db { class Database; }

class QThread;

namespace media {

class ThumbnailCache;

/**
 * @class ThumbnailScanner
 * @brief Generates thumbnails ahead of demand, when nothing better is happening.
 *
 * The first visit to a cold folder is slow however fast generation is, because
 * every image is a miss. This walks the library in the background so the misses
 * have already happened by the time anyone looks.
 *
 * ### Yielding is the whole design
 *
 * A warm-up pass must never compete with someone actually using the server. It
 * runs on one low-priority thread, and before every file it asks whether any
 * transfer is active; if so it waits rather than working. Getting this wrong would
 * make the feature worse than not having it — a background job that stutters
 * someone's video is a regression, not an optimisation.
 *
 * It also stops on battery power by default, since the target hardware is a
 * laptop and walking a large library would drain it.
 *
 * ### Progress survives restarts
 *
 * Completed directories are recorded in the `scan_state` table with a timestamp,
 * so a restart resumes instead of rescanning. A directory is revisited when its
 * modification time is newer than the recorded sweep, which is how new photos get
 * picked up without a full walk.
 */
class ThumbnailScanner : public QObject
{
    Q_OBJECT

public:
    /**
     * @param cache    Generates and stores thumbnails. Must outlive this object.
     * @param database Holds scan progress. May be null, in which case every start
     *                 rescans from the beginning.
     */
    ThumbnailScanner(ThumbnailCache* cache,
                     db::Database*   database,
                     QObject*        parent = nullptr);
    ~ThumbnailScanner() override;

    struct Progress
    {
        int  directoriesVisited = 0;
        int  filesExamined      = 0;
        int  thumbnailsCreated  = 0;
        int  directoriesQueued  = 0;
        bool waiting            = false;  ///< Yielding to a transfer, or on battery.
        bool running            = false;
    };

    /** Root to walk. Set before start(). */
    void setRoot(const QString& root);

    /**
     * @brief Supplies the "is the server busy" test.
     *
     * Injected rather than depending on the HTTP layer directly, which keeps this
     * testable without standing up a server. Called from the scanner thread, so
     * the predicate must be thread-safe — AdmissionControl::activeStreams() is.
     */
    void setBusyPredicate(std::function<bool()> predicate);

    /** Whether to stop while running on battery. Default true. */
    void setPauseOnBattery(bool pause);

    /** Starts or resumes the walk. Safe to call when already running. */
    void start();

    /** Stops after the current file. Safe to call from any thread. */
    void stop();

    bool     isRunning() const;
    Progress progress() const;

signals:
    void progressChanged(media::ThumbnailScanner::Progress progress);
    void finished();

private:
    /** Runs the whole walk on the scanner thread. No event loop involved. */
    void runWalk();

    void enqueueChildren(const QString& directoryPath);
    bool shouldYield() const;

    bool directoryNeedsSweep(const QString& directoryPath) const;
    void recordDirectorySwept(const QString& directoryPath);

    ThumbnailCache* m_cache    = nullptr;
    db::Database*   m_database = nullptr;

    QString  m_root;
    QThread* m_thread = nullptr;   ///< Recreated per run; joined before deletion.

    QQueue<QString> m_pendingDirectories;
    QQueue<QString> m_pendingFiles;

    /**
     * Canonical paths already walked, and the canonical root.
     *
     * A second line of defence against revisiting a directory. The primary guard
     * is skipping links altogether in enqueueChildren(): canonicalFilePath() does
     * not resolve an NTFS junction, so it cannot be relied on to detect a cycle on
     * Windows. The root comparison additionally keeps the walk inside the served
     * tree, where PathResolver would refuse to serve anything outside it anyway.
     */
    QSet<QString> m_visitedDirectories;
    QString       m_rootCanonical;

    std::function<bool()> m_busyPredicate;
    bool                  m_pauseOnBattery = true;

    mutable QMutex m_mutex;              ///< Guards m_progress and the settings.
    Progress       m_progress;

    /// Atomic rather than mutex-guarded: read on every loop iteration, and set
    /// from whichever thread asks the scan to stop.
    QAtomicInteger<bool> m_stopRequested = false;
};

} // namespace media

Q_DECLARE_METATYPE(media::ThumbnailScanner::Progress)
