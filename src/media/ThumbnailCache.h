#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

namespace db { class Database; }

class QThreadPool;

namespace media {

/**
 * @brief Which size of thumbnail is wanted.
 *
 * Two tiers rather than one. Small backs the grid; Medium backs the preview
 * overlay, which previously loaded the original — several megabytes to look at one
 * phone photo. Medium is the difference between a preview costing ~120 KB and
 * ~5 MB, and matters as much as the grid tier on a slow link.
 */
enum class ThumbTier
{
    Small,    ///< 320 px long edge. Grid cards.
    Medium,   ///< 1280 px long edge. Preview overlay.
};

/** Long edge in pixels for @p tier. */
int tierLongEdge(ThumbTier tier);

/** Single character used in cache keys and URLs. */
char tierCode(ThumbTier tier);

/** Parses a tier code, or returns nullopt. */
std::optional<ThumbTier> tierFromCode(char code);

/**
 * @brief Stable cache key for one source file at one tier.
 *
 * Includes modification time and size, so a changed file produces a different key.
 * That is what removes invalidation logic entirely, and what makes it safe to
 * serve thumbnails with an immutable cache policy: a new file version is simply a
 * different URL.
 *
 * Pure, so it can be tested without touching a filesystem.
 */
QByteArray thumbnailKey(const QString& sourcePath,
                        qint64         modifiedMsecs,
                        qint64         sourceSize,
                        ThumbTier      tier);

/**
 * @brief Short tag placed in thumbnail URLs as ?v=.
 *
 * Derived from the same inputs as the key, so it changes when the file changes.
 * Its only job is to make the URL change; the server resolves the real path from
 * the URL itself rather than from this value.
 */
QByteArray thumbnailVersionTag(const QString& sourcePath,
                               qint64         modifiedMsecs,
                               qint64         sourceSize,
                               ThumbTier      tier);

/**
 * @brief Key identifying one source file version for the "cannot decode" record.
 *
 * Deliberately tier-independent: a file Qt cannot read is unreadable at every
 * size, so remembering it once covers both tiers. Built from the same path, mtime
 * and size as thumbnailKey(), so replacing the file retries it automatically.
 */
QByteArray decodeFailureKey(const QString& sourcePath,
                            qint64         modifiedMsecs,
                            qint64         sourceSize);

/**
 * @class ThumbnailCache
 * @brief Generates and caches downscaled copies of images.
 *
 * This is the change that addresses the original complaint. A folder of sixty
 * phone photos previously transferred roughly 300 MB to paint sixty 160x110
 * boxes, because each grid image pointed at the full-resolution original.
 * loading="lazy" deferred that cost without removing it.
 *
 * ### Never blocks a worker
 *
 * On a cache miss the requesting worker thread returns immediately and the
 * response is sent later, from a signal. With four workers and sixty misses,
 * blocking would serialise an entire page behind the decode queue.
 *
 * Requests for the same key are coalesced, so two browser tabs on one folder
 * decode each image once rather than twice.
 *
 * Thread-safe; one instance is shared by every worker.
 */
class ThumbnailCache : public QObject
{
    Q_OBJECT

public:
    /** Default ceiling for the on-disk cache. */
    static constexpr qint64 DefaultMaxBytes = 2LL * 1024 * 1024 * 1024;

    /**
     * @param database        Index of generated thumbnails. May be null, in which
     *                        case the cache still works but cannot evict by least
     *                        recent use.
     * @param cacheDirectory  Root for generated files; created if missing.
     * @param generatorThreads Concurrent decodes, from the hardware tier.
     */
    ThumbnailCache(db::Database* database,
                   QString       cacheDirectory,
                   int           generatorThreads,
                   QObject*      parent = nullptr);
    ~ThumbnailCache() override;

    /** True when this path looks like an image Qt can decode. */
    static bool isSupportedImage(const QString& path);

    /**
     * @brief True when this exact file version has already failed to decode.
     *
     * The extension is only a hint. An iPhone backup contains QuickTime movies and
     * XML sidecars named `.JPG`, and without this every folder visit re-read
     * megabytes off the disk to fail again. Consulted by the listing API as well,
     * so the client is never handed a thumbnail URL that cannot be satisfied.
     *
     * Reads an in-memory set; cheap enough for the hot path.
     */
    bool isKnownUndecodable(const QString& sourcePath,
                            qint64         modifiedMsecs,
                            qint64         sourceSize) const;

    /** Same test, stat'ing the file itself. Returns false when it does not exist. */
    bool isKnownUndecodable(const QString& sourcePath) const;

    /**
     * @brief Returns the cached file for @p sourcePath, if it already exists.
     *
     * One stat call. Does not generate anything.
     */
    std::optional<QString> hit(const QString& sourcePath, ThumbTier tier);

    /**
     * @brief Ensures a thumbnail exists, then calls @p handler.
     *
     * @param context Owns the handler's lifetime. If it is destroyed first, the
     *                handler is never called — which is the point: the requesting
     *                connection may well die while a decode is queued, and Qt's
     *                own receiver-based disconnection is the only race-free way to
     *                handle that.
     *
     * The handler runs in @p context's thread. Called immediately on a hit.
     */
    void requestAsync(const QString& sourcePath,
                      ThumbTier      tier,
                      QObject*       context,
                      std::function<void(bool ok, const QString& filePath)> handler);

    /**
     * @brief Generates a thumbnail on the calling thread, blocking until done.
     *
     * For the background scanner, which has its own low-priority thread and paces
     * itself. Routing its work through the request pool instead would let a warm-up
     * pass queue ahead of a thumbnail someone is actually waiting for.
     *
     * Returns false without generating when another request for the same key is
     * already in flight — that one will produce the file.
     *
     * @return true when a thumbnail exists afterwards.
     */
    bool generateBlocking(const QString& sourcePath, ThumbTier tier);

    void   setMaxBytes(qint64 bytes);
    qint64 maxBytes() const;
    qint64 totalBytes() const;

    /** Deletes least recently used entries until comfortably under the ceiling. */
    void evictIfNeeded();

    /** Format generated files use, decided once from Qt's available writers. */
    QByteArray outputFormat() const { return m_outputFormat; }

signals:
    /** Emitted when a requested key finishes, successfully or not. */
    void ready(const QByteArray& key, bool ok, const QString& filePath);

private:
    QString cacheFilePath(const QByteArray& key) const;

    /** Runs on a generator thread. */
    void generate(const QByteArray& key,
                  const QString&    sourcePath,
                  ThumbTier         tier,
                  const QString&    destinationPath);

    void recordInIndex(const QByteArray& key, const QString& sourcePath,
                       qint64 modifiedMsecs, qint64 sourceSize,
                       ThumbTier tier, qint64 bytes);
    void touchInIndex(const QByteArray& key);
    void loadTotalBytesFromIndex();

    void recordDecodeFailure(const QString& sourcePath, const QString& reason);
    void loadDecodeFailures();

    db::Database* m_database = nullptr;
    QString       m_cacheDirectory;
    QByteArray    m_outputFormat;      ///< "webp", or "jpeg" when WebP is absent.
    int           m_quality = 72;

    QThreadPool* m_pool = nullptr;

    mutable QMutex m_mutex;
    QSet<QByteArray> m_inFlight;       ///< Keys currently being generated.

    /**
     * File versions known not to decode, so they are attempted once and never
     * again. Held in memory because the listing API consults it per entry; the
     * table behind it is the durable copy.
     */
    QSet<QByteArray> m_undecodable;

    qint64 m_totalBytes = 0;
    qint64 m_maxBytes   = DefaultMaxBytes;
};

} // namespace media
