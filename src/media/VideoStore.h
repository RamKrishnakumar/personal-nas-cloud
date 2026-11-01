#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QWaitCondition>

#include <optional>

namespace db { class Database; }

class QThread;

namespace media {

class VideoTranscoder;

/** Where a source stands with respect to having a streamable rendition. */
enum class VideoState
{
    Unknown,    ///< Never looked at. The listing shows nothing special.
    NotNeeded,  ///< Probed, and the original already streams fine.
    Queued,     ///< Waiting for the worker.
    Running,    ///< Being transcoded now; percent is meaningful.
    Ready,      ///< A rendition exists and can be served.
    Failed,     ///< Probing or transcoding failed. Not retried.
};

QString videoStateToString(VideoState state);

struct VideoStatus
{
    VideoState state   = VideoState::Unknown;
    int        percent = 0;      ///< Only while Running.
    QString    reason;           ///< Why skipped or failed, for the interface.
    int        queueAhead = 0;   ///< Jobs in front of this one, while Queued.
};

/**
 * @class VideoStore
 * @brief Keeps transcoded renditions, and makes exactly one at a time.
 *
 * ### Lazy, because of what it costs
 *
 * Transcoding runs at 0.23x realtime on the reference machine — four minutes of work
 * per minute of video, dominated by software HEVC decode that no setting improves
 * (§16.2). Converting a library up front would occupy the machine for hours nobody
 * asked for, so nothing happens until someone tries to play a file. The optional
 * background pass exists for the person who *does* want to spend the night on it.
 *
 * ### One job at a time, and that is not a placeholder
 *
 * The worker is a single thread. Two concurrent transcodes on two cores would each
 * take twice as long while the server lost the core it needs to stay responsive, and
 * the person waiting would see both bars crawl. Queueing is the honest version: one
 * job moves at full speed and the queue depth is reported so the interface can say
 * "2 ahead of you" rather than appearing stuck.
 *
 * ### What is remembered, and why that matters more than it looks
 *
 * Every verdict is durable, including the negative ones. `skipped` means assess()
 * looked and the original already streams fine; `failed` means it cannot be helped.
 * Both are recorded because *probing is itself a subprocess* — without them, every
 * visit to a folder of videos would spawn one per file, forever.
 *
 * Identity is the usual path/mtime/size triple, so replacing a file invalidates its
 * rendition by producing a different key, with no invalidation step anywhere.
 *
 * Thread-safe. The worker holds the mutex only to move state, never while encoding.
 */
class VideoStore : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Default ceiling for renditions on disk.
     *
     * Larger than the thumbnail cache by two orders of magnitude, because a
     * rendition is about 10 MB per minute of video against a thumbnail's 20 KB.
     * Eight gigabytes is roughly thirteen hours of video.
     */
    static constexpr qint64 DefaultMaxBytes = 8LL * 1024 * 1024 * 1024;

    VideoStore(db::Database*          database,
               const VideoTranscoder* transcoder,
               QString                cacheDirectory,
               QObject*               parent = nullptr);
    ~VideoStore() override;

    /** False when there is no ffmpeg, in which case everything reports Unknown. */
    bool isAvailable() const;

    /** True for an extension worth offering optimisation for. */
    static bool handles(const QString& path);

    /**
     * @brief Where @p sourcePath stands. Does not start any work.
     *
     * Cheap: an in-memory check for live jobs, then one indexed row. Safe to call
     * per entry while building a listing.
     */
    VideoStatus status(const QString& sourcePath) const;

    /**
     * @brief Asks for a rendition, queueing one if needed, and reports where it got.
     *
     * Idempotent — asking twice for the same file joins the existing job rather than
     * starting a second one, which is what makes the client free to poll.
     */
    VideoStatus request(const QString& sourcePath);

    /** The rendition's file path when one is Ready, touching its LRU timestamp. */
    std::optional<QString> readyPath(const QString& sourcePath);

    /**
     * @brief Queues every video beneath @p directory that wants one.
     *
     * The optional background pass. Probing happens on the worker, not here, so a
     * large tree does not block the caller — and the caller is the GUI thread.
     *
     * @return how many were added to the queue.
     */
    int enqueueTree(const QString& directory);

    /** Jobs waiting, plus the one running. */
    int pending() const;

    /** Abandons everything not yet started, and the running job. */
    void cancelAll();

    void   setMaxBytes(qint64 bytes);
    qint64 maxBytes() const;
    qint64 totalBytes() const;

    /** Deletes least-recently-used renditions until under the ceiling. */
    void evictIfNeeded();

signals:
    /** A job reached a terminal state. @p ok is false for Failed. */
    void finished(const QString& sourcePath, bool ok);

    /** Emitted as a running job advances, for the desktop window. */
    void progress(const QString& sourcePath, int percent);

private:
    struct Job
    {
        QString    sourcePath;
        QByteArray key;
        qint64     modifiedMsecs = 0;
        qint64     sourceSize    = 0;
    };

    /** The worker loop. One thread, started on construction. */
    void work();

    /** Runs one job: probe, assess, transcode. Called on the worker thread. */
    void process(const Job& job);

    QString renditionPath(const QByteArray& key) const;

    /** Identity of @p sourcePath at its current version, or empty if it is gone. */
    QByteArray keyFor(const QString& sourcePath,
                      qint64*        modifiedMsecs = nullptr,
                      qint64*        sourceSize    = nullptr) const;

    // ── Index ─────────────────────────────────────────────────────────────
    std::optional<VideoStatus> readIndex(const QByteArray& key) const;
    void recordState(const QByteArray& key, const Job& job, const QString& state,
                     const QString& reason, qint64 bytes, qint64 durationMs);
    void touchIndex(const QByteArray& key);
    void loadTotalBytesFromIndex();

    db::Database*          m_database   = nullptr;
    const VideoTranscoder* m_transcoder = nullptr;
    QString                m_cacheDirectory;

    QThread* m_worker = nullptr;

    mutable QMutex  m_mutex;
    QWaitCondition  m_wakeUp;

    QQueue<Job>            m_queue;
    QHash<QByteArray, int> m_queuedKeys;   ///< Key -> position, to avoid duplicates.

    QByteArray m_runningKey;
    QString    m_runningSource;
    int        m_runningPercent = 0;

    bool m_cancelRunning = false;
    bool m_stopping      = false;

    qint64 m_totalBytes = 0;
    qint64 m_maxBytes   = DefaultMaxBytes;
};

} // namespace media
