#include "media/ThumbnailCache.h"

#include "db/Database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QMetaObject>
#include <QSqlQuery>
#include <QThreadPool>

#include <algorithm>

namespace {

/** Decoded images above this are refused, to bound peak memory per decode. */
constexpr qint64 MaxSourcePixels = 80LL * 1000 * 1000;

/** Fraction of the ceiling to shrink to when evicting, so it is not run per file. */
constexpr double EvictionTargetRatio = 0.8;

QByteArray hashOf(const QString& sourcePath,
                  qint64         modifiedMsecs,
                  qint64         sourceSize,
                  char           tierCode)
{
    QCryptographicHash hash(QCryptographicHash::Md5);
    // Normalised so the same file reached through different separators or cases on
    // a case-insensitive filesystem does not generate two copies.
    hash.addData(QDir::cleanPath(sourcePath).toCaseFolded().toUtf8());
    hash.addData(QByteArrayLiteral("|"));
    hash.addData(QByteArray::number(modifiedMsecs));
    hash.addData(QByteArrayLiteral("|"));
    hash.addData(QByteArray::number(sourceSize));
    hash.addData(QByteArrayLiteral("|"));
    hash.addData(QByteArray(1, tierCode));
    return hash.result().toHex();
}

} // namespace

namespace media {

// ─────────────────────────────────────────────────────────────────────────────
//  Pure helpers
// ─────────────────────────────────────────────────────────────────────────────

int tierLongEdge(ThumbTier tier)
{
    switch (tier) {
    case ThumbTier::Small:  return 320;
    case ThumbTier::Medium: return 1280;
    }
    return 320;
}

char tierCode(ThumbTier tier)
{
    return tier == ThumbTier::Medium ? 'm' : 's';
}

std::optional<ThumbTier> tierFromCode(char code)
{
    switch (code) {
    case 's': return ThumbTier::Small;
    case 'm': return ThumbTier::Medium;
    default:  return std::nullopt;
    }
}

QByteArray thumbnailKey(const QString& sourcePath, qint64 modifiedMsecs,
                        qint64 sourceSize, ThumbTier tier)
{
    return hashOf(sourcePath, modifiedMsecs, sourceSize, tierCode(tier));
}

QByteArray thumbnailVersionTag(const QString& sourcePath, qint64 modifiedMsecs,
                               qint64 sourceSize, ThumbTier tier)
{
    // A prefix of the key: shorter URLs, and it changes exactly when the key does.
    return thumbnailKey(sourcePath, modifiedMsecs, sourceSize, tier).left(12);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────

ThumbnailCache::ThumbnailCache(db::Database* database,
                               QString       cacheDirectory,
                               int           generatorThreads,
                               QObject*      parent)
    : QObject(parent)
    , m_database(database)
    , m_cacheDirectory(std::move(cacheDirectory))
{
    QDir().mkpath(m_cacheDirectory);

    // Decided once. WebP is roughly 30% smaller than JPEG at equivalent quality,
    // but Qt's qwebp plugin is not guaranteed to be present in every build, and
    // discovering that per-file would mean a mix of formats in one cache.
    const auto writable = QImageWriter::supportedImageFormats();
    if (writable.contains(QByteArrayLiteral("webp"))) {
        m_outputFormat = QByteArrayLiteral("webp");
        m_quality      = 72;
        qInfo() << "[Thumbs] Using WebP.";
    } else {
        m_outputFormat = QByteArrayLiteral("jpeg");
        m_quality      = 80;
        qInfo() << "[Thumbs] WebP writer unavailable; using JPEG (about 30% larger). "
                   "Install Qt's \"Qt Image Formats\" component to enable WebP, "
                   "then delete this cache directory so thumbnails are rebuilt.";
    }

    // Logged because it determines which files get a thumbnail at all. HEIC is the
    // common surprise: it is an image, but no Qt plugin reads it without an
    // additional decoder.
    qInfo() << "[Thumbs] Readable formats:"
            << QImageReader::supportedImageFormats().join(' ').constData();

    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(std::max(1, generatorThreads));

    // Decoding must lose to serving. QThreadPool offers no thread priority, so the
    // thread count is the real control here; the Scanner added in step 12 also
    // yields while any transfer is active.
    m_pool->setObjectName(QStringLiteral("nestdrive-thumbs"));

    loadTotalBytesFromIndex();
    evictIfNeeded();
}

ThumbnailCache::~ThumbnailCache()
{
    // Generators hold a raw `this`, so they must finish before the object dies.
    m_pool->clear();
    m_pool->waitForDone();
}

bool ThumbnailCache::isSupportedImage(const QString& path)
{
    static const QSet<QByteArray> readable = [] {
        QSet<QByteArray> formats;
        for (const QByteArray& format : QImageReader::supportedImageFormats())
            formats.insert(format.toLower());
        return formats;
    }();

    return readable.contains(QFileInfo(path).suffix().toLower().toUtf8());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lookup and request
// ─────────────────────────────────────────────────────────────────────────────

QString ThumbnailCache::cacheFilePath(const QByteArray& key) const
{
    // Two-character shard directory. Not cosmetic: a hundred thousand files in one
    // directory makes enumeration pathological on both NTFS and ext4, and a large
    // library reaches that easily.
    const QString hex = QString::fromLatin1(key);
    return m_cacheDirectory + QLatin1Char('/') + hex.left(2) + QLatin1Char('/')
           + hex + QLatin1Char('.') + QString::fromLatin1(m_outputFormat);
}

std::optional<QString> ThumbnailCache::hit(const QString& sourcePath, ThumbTier tier)
{
    const QFileInfo info(sourcePath);
    if (!info.exists())
        return std::nullopt;

    const QByteArray key = thumbnailKey(sourcePath,
                                        info.lastModified().toMSecsSinceEpoch(),
                                        info.size(), tier);
    const QString path = cacheFilePath(key);

    if (!QFileInfo::exists(path))
        return std::nullopt;

    touchInIndex(key);
    return path;
}

void ThumbnailCache::requestAsync(const QString& sourcePath,
                                  ThumbTier      tier,
                                  QObject*       context,
                                  std::function<void(bool, const QString&)> handler)
{
    if (const auto existing = hit(sourcePath, tier)) {
        handler(true, *existing);
        return;
    }

    const QFileInfo info(sourcePath);
    if (!info.exists() || !isSupportedImage(sourcePath)) {
        handler(false, QString());
        return;
    }

    const qint64     modified = info.lastModified().toMSecsSinceEpoch();
    const QByteArray key      = thumbnailKey(sourcePath, modified, info.size(), tier);
    const QString    target   = cacheFilePath(key);

    // Connected before the job is queued, or a fast generator could finish first
    // and the signal would be missed.
    //
    // The receiver is `context` rather than `this`, so Qt both delivers in the
    // requester's thread and drops the connection if the requester dies. A
    // connection may well outlive the socket that asked for it.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &ThumbnailCache::ready, context,
        [this, key, handler, connection](const QByteArray& readyKey,
                                         bool ok, const QString& filePath) {
            if (readyKey != key)
                return;
            QObject::disconnect(*connection);     // One shot.
            handler(ok, filePath);
        });

    {
        QMutexLocker locker(&m_mutex);
        if (m_inFlight.contains(key))
            return;                 // Already queued; this requester now waits too.
        m_inFlight.insert(key);
    }

    m_pool->start([this, key, sourcePath, tier, target]() {
        generate(key, sourcePath, tier, target);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Generation
// ─────────────────────────────────────────────────────────────────────────────

void ThumbnailCache::generate(const QByteArray& key,
                              const QString&    sourcePath,
                              ThumbTier         tier,
                              const QString&    destinationPath)
{
    bool    ok = false;
    QString produced;

    const int longEdge = tierLongEdge(tier);

    QImageReader reader(sourcePath);

    // Honours EXIF orientation. Without it, photos taken in portrait appear
    // rotated, which is the single most visible thumbnail defect.
    reader.setAutoTransform(true);

    const QSize original = reader.size();

    if (original.isValid()
        && qint64(original.width()) * original.height() <= MaxSourcePixels) {
        // The important optimisation. For JPEG this lets libjpeg decode at 1/2,
        // 1/4 or 1/8 scale directly, which is several times faster than decoding
        // at full resolution and shrinking afterwards, and uses proportionally
        // less memory. On the reference CPU that is the difference between
        // thumbnailing a library in minutes rather than hours.
        if (original.width() > longEdge || original.height() > longEdge) {
            reader.setScaledSize(original.scaled(longEdge, longEdge,
                                                 Qt::KeepAspectRatio));
        }
    }

    QImage image = reader.read();

    if (!image.isNull()) {
        // Safety net: formats without a scaled-decode path, and the case where an
        // EXIF transform swaps width and height after scaling, can both come back
        // larger than asked.
        if (image.width() > longEdge || image.height() > longEdge) {
            image = image.scaled(longEdge, longEdge,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }

        QDir().mkpath(QFileInfo(destinationPath).absolutePath());

        // Written to a temporary name and renamed, so a crash or a concurrent
        // reader never sees a half-written thumbnail as a valid cache entry.
        const QString temporaryPath = destinationPath + QStringLiteral(".part");

        // The writer is scoped so it closes the file before the rename below.
        // Windows refuses to rename a file that is still open, and QImageWriter
        // holds its device until destroyed — which fails silently if the rename
        // result is not checked.
        bool written = false;
        {
            QImageWriter writer(temporaryPath, m_outputFormat);
            writer.setQuality(m_quality);
            written = writer.write(image);
            if (!written)
                qWarning() << "[Thumbs] Write failed:" << writer.errorString();
        }

        if (written) {
            QFile::remove(destinationPath);
            if (QFile::rename(temporaryPath, destinationPath)) {
                ok       = true;
                produced = destinationPath;
            } else {
                qWarning() << "[Thumbs] Could not move thumbnail into place:"
                           << temporaryPath << "->" << destinationPath;
                QFile::remove(temporaryPath);
            }
        } else {
            QFile::remove(temporaryPath);
        }
    } else {
        qWarning() << "[Thumbs] Decode failed for" << sourcePath
                   << ':' << reader.errorString();
    }

    if (ok) {
        const QFileInfo sourceInfo(sourcePath);
        recordInIndex(key, sourcePath,
                      sourceInfo.lastModified().toMSecsSinceEpoch(),
                      sourceInfo.size(), tier, QFileInfo(produced).size());
        evictIfNeeded();
    }

    {
        QMutexLocker locker(&m_mutex);
        m_inFlight.remove(key);
    }

    emit ready(key, ok, produced);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Index and eviction
// ─────────────────────────────────────────────────────────────────────────────

void ThumbnailCache::recordInIndex(const QByteArray& key, const QString& sourcePath,
                                   qint64 modifiedMsecs, qint64 sourceSize,
                                   ThumbTier tier, qint64 bytes)
{
    {
        QMutexLocker locker(&m_mutex);
        m_totalBytes += bytes;
    }

    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO thumbs "
        "(key, src_path, mtime_ms, size, tier, bytes, created_at, last_used_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    query.addBindValue(QString::fromLatin1(key));
    query.addBindValue(sourcePath);
    query.addBindValue(modifiedMsecs);
    query.addBindValue(sourceSize);
    query.addBindValue(QString(QChar(tierCode(tier))));
    query.addBindValue(bytes);
    query.addBindValue(now);
    query.addBindValue(now);
    query.exec();
}

void ThumbnailCache::touchInIndex(const QByteArray& key)
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());

    // Only written when the recorded value is over an hour old. A folder view
    // produces dozens of hits at once, and a write per hit would turn browsing
    // into a burst of database traffic for no benefit — hour granularity is far
    // finer than eviction needs.
    query.prepare(QStringLiteral(
        "UPDATE thumbs SET last_used_at = ? WHERE key = ? AND last_used_at < ?"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    query.addBindValue(now);
    query.addBindValue(QString::fromLatin1(key));
    query.addBindValue(now - 3600);
    query.exec();
}

void ThumbnailCache::loadTotalBytesFromIndex()
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (query.exec(QStringLiteral("SELECT COALESCE(SUM(bytes), 0) FROM thumbs"))
        && query.next()) {
        QMutexLocker locker(&m_mutex);
        m_totalBytes = query.value(0).toLongLong();
    }
}

void ThumbnailCache::setMaxBytes(qint64 bytes)
{
    {
        QMutexLocker locker(&m_mutex);
        m_maxBytes = std::max<qint64>(0, bytes);
    }
    evictIfNeeded();
}

qint64 ThumbnailCache::maxBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_maxBytes;
}

qint64 ThumbnailCache::totalBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_totalBytes;
}

void ThumbnailCache::evictIfNeeded()
{
    qint64 ceiling = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_totalBytes <= m_maxBytes)
            return;
        ceiling = m_maxBytes;
    }

    if (!m_database || !m_database->isOpen())
        return;        // Without the index there is no least-recently-used order.

    const qint64 target = qint64(ceiling * EvictionTargetRatio);

    while (true) {
        QString    victimKey;
        qint64     victimBytes = 0;

        {
            QMutexLocker locker(&m_mutex);
            if (m_totalBytes <= target)
                return;
        }

        {
            auto handle = m_database->lock();
            QSqlQuery query(handle.connection());
            if (!query.exec(QStringLiteral(
                    "SELECT key, bytes FROM thumbs ORDER BY last_used_at ASC LIMIT 1"))
                || !query.next()) {
                return;
            }
            victimKey   = query.value(0).toString();
            victimBytes = query.value(1).toLongLong();

            QSqlQuery remove(handle.connection());
            remove.prepare(QStringLiteral("DELETE FROM thumbs WHERE key = ?"));
            remove.addBindValue(victimKey);
            remove.exec();
        }

        QFile::remove(cacheFilePath(victimKey.toLatin1()));

        QMutexLocker locker(&m_mutex);
        m_totalBytes = std::max<qint64>(0, m_totalBytes - victimBytes);
    }
}

} // namespace media
