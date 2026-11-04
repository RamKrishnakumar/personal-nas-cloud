#include "media/VideoStore.h"

#include "db/Database.h"
#include "media/FrameExtractor.h"
#include "media/VideoTranscoder.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSqlQuery>
#include <QThread>

namespace {

/**
 * Directories never walked looking for videos.
 *
 * Our own cache is the one that matters: renditions are `.mp4`, so a background pass
 * would otherwise find them, decide they are sources, and transcode its own output.
 */
bool isSkippableDirectory(const QString& name)
{
    return name.startsWith(QLatin1Char('.'))
        || name == QLatin1String("$RECYCLE.BIN")
        || name == QLatin1String("System Volume Information");
}

} // namespace

namespace media {

QString videoStateToString(VideoState state)
{
    switch (state) {
    case VideoState::Unknown:   return QStringLiteral("unknown");
    case VideoState::NotNeeded: return QStringLiteral("notNeeded");
    case VideoState::Queued:    return QStringLiteral("queued");
    case VideoState::Running:   return QStringLiteral("running");
    case VideoState::Ready:     return QStringLiteral("ready");
    case VideoState::Failed:    return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

bool VideoStore::handles(const QString& path)
{
    // The same list the thumbnailer uses, so the two cannot disagree about what a
    // video is.
    return FrameExtractor::isVideo(path);
}

VideoStore::VideoStore(db::Database*          database,
                       const VideoTranscoder* transcoder,
                       QString                cacheDirectory,
                       QObject*               parent)
    : QObject(parent)
    , m_database(database)
    , m_transcoder(transcoder)
    , m_cacheDirectory(std::move(cacheDirectory))
{
    QDir().mkpath(m_cacheDirectory);
    loadTotalBytesFromIndex();

    if (!isAvailable())
        return;

    m_worker = QThread::create([this] { work(); });
    m_worker->setObjectName(QStringLiteral("video-transcoder"));
    m_worker->start();

    // Below normal, so a transcode does not compete with serving files. The whole
    // point of this design is that the machine stays usable while it runs; four
    // minutes per minute of video is a long time to make everything else sluggish.
    m_worker->setPriority(QThread::LowPriority);
}

VideoStore::~VideoStore()
{
    {
        QMutexLocker locker(&m_mutex);
        m_stopping      = true;
        m_cancelRunning = true;
        m_queue.clear();
        m_queuedKeys.clear();
        m_wakeUp.wakeAll();
    }

    if (m_worker) {
        // The running transcode notices m_cancelRunning through its progress
        // callback and kills its process, so this does not wait for a four-minute
        // encode to finish on its own.
        m_worker->wait();
        delete m_worker;
    }
}

bool VideoStore::isAvailable() const
{
    return m_transcoder && m_transcoder->isAvailable();
}

QString VideoStore::renditionPath(const QByteArray& key) const
{
    // Two-character shard, as the thumbnail cache does: a flat directory of tens of
    // thousands of files makes enumeration pathological on NTFS and ext4 alike.
    const QString hex = QString::fromLatin1(key);
    return m_cacheDirectory + QLatin1Char('/') + hex.left(2) + QLatin1Char('/')
           + hex + QStringLiteral(".mp4");
}

QByteArray VideoStore::keyFor(const QString& sourcePath, qint64* modifiedMsecs,
                              qint64* sourceSize) const
{
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile())
        return {};

    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    const qint64 size  = info.size();

    if (modifiedMsecs)
        *modifiedMsecs = mtime;
    if (sourceSize)
        *sourceSize = size;

    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(QDir::cleanPath(sourcePath).toCaseFolded().toUtf8());
    hash.addData(QByteArrayLiteral("|"));
    hash.addData(QByteArray::number(mtime));
    hash.addData(QByteArrayLiteral("|"));
    hash.addData(QByteArray::number(size));

    // 'v' so a rendition key can never collide with a thumbnail key for the same
    // file, which shares the cache root.
    hash.addData(QByteArrayLiteral("|v1080"));

    return hash.result().toHex();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Status
// ─────────────────────────────────────────────────────────────────────────────

VideoStatus VideoStore::status(const QString& sourcePath) const
{
    VideoStatus result;

    if (!isAvailable() || !handles(sourcePath))
        return result;

    const QByteArray key = keyFor(sourcePath);
    if (key.isEmpty())
        return result;

    {
        QMutexLocker locker(&m_mutex);

        if (key == m_runningKey) {
            result.state   = VideoState::Running;
            result.percent = m_runningPercent;
            return result;
        }

        const auto queued = m_queuedKeys.constFind(key);
        if (queued != m_queuedKeys.cend()) {
            result.state = VideoState::Queued;

            // Counted live rather than stored, because everything in front shifts
            // forward as jobs complete.
            int ahead = 0;
            for (const Job& job : m_queue) {
                if (job.key == key)
                    break;
                ++ahead;
            }
            result.queueAhead = ahead + (m_runningKey.isEmpty() ? 0 : 1);
            return result;
        }
    }

    if (const auto recorded = readIndex(key))
        return *recorded;

    return result;
}

std::optional<VideoStatus> VideoStore::readIndex(const QByteArray& key) const
{
    if (!m_database || !m_database->isOpen())
        return std::nullopt;

    QString state;
    QString reason;
    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral("SELECT state, reason FROM videos WHERE key = ?"));
        query.addBindValue(QString::fromLatin1(key));

        if (!query.exec() || !query.next())
            return std::nullopt;

        state  = query.value(0).toString();
        reason = query.value(1).toString();
    }

    VideoStatus result;
    result.reason = reason;

    if (state == QLatin1String("ready")) {
        // Checked against the filesystem, not trusted. A rendition deleted by hand,
        // or lost to a cache wipe, must read as absent rather than as ready — the
        // alternative is a listing that offers a URL answering 404.
        if (!QFileInfo::exists(renditionPath(key)))
            return std::nullopt;
        result.state = VideoState::Ready;
    } else if (state == QLatin1String("skipped")) {
        result.state = VideoState::NotNeeded;
    } else {
        result.state = VideoState::Failed;
    }

    return result;
}

std::optional<QString> VideoStore::readyPath(const QString& sourcePath)
{
    if (!isAvailable())
        return std::nullopt;

    const QByteArray key = keyFor(sourcePath);
    if (key.isEmpty())
        return std::nullopt;

    const auto recorded = readIndex(key);
    if (!recorded || recorded->state != VideoState::Ready)
        return std::nullopt;

    touchIndex(key);
    return renditionPath(key);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Requesting
// ─────────────────────────────────────────────────────────────────────────────

VideoStatus VideoStore::request(const QString& sourcePath)
{
    const VideoStatus existing = status(sourcePath);

    // Already answered, one way or another. Failed is included deliberately: a file
    // that could not be transcoded will not be transcoded by asking again, and
    // retrying would spend four minutes per minute of video to fail identically.
    if (existing.state != VideoState::Unknown)
        return existing;

    if (!isAvailable() || !handles(sourcePath))
        return existing;

    Job job;
    job.sourcePath = sourcePath;
    job.key = keyFor(sourcePath, &job.modifiedMsecs, &job.sourceSize);
    if (job.key.isEmpty())
        return existing;

    VideoStatus queued;
    queued.state = VideoState::Queued;

    {
        QMutexLocker locker(&m_mutex);

        if (m_stopping)
            return existing;

        // Re-checked under the lock: two clients polling at once would otherwise
        // both pass the status() check above and queue the same file twice.
        if (job.key == m_runningKey) {
            queued.state   = VideoState::Running;
            queued.percent = m_runningPercent;
            return queued;
        }
        if (m_queuedKeys.contains(job.key)) {
            queued.queueAhead = m_queue.size();
            return queued;
        }

        m_queuedKeys.insert(job.key, m_queue.size());
        m_queue.enqueue(job);
        queued.queueAhead = m_queue.size() - 1 + (m_runningKey.isEmpty() ? 0 : 1);

        m_wakeUp.wakeOne();
    }

    qInfo() << "[Video] Queued" << QFileInfo(sourcePath).fileName()
            << "— position" << queued.queueAhead;

    return queued;
}

int VideoStore::enqueueTree(const QString& directory)
{
    if (!isAvailable())
        return 0;

    int added = 0;

    QDirIterator walker(directory,
                        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

    while (walker.hasNext()) {
        const QString path = walker.next();
        const QFileInfo info = walker.fileInfo();

        if (info.isDir()) {
            if (isSkippableDirectory(info.fileName()))
                continue;
            continue;
        }

        if (!handles(path))
            continue;

        if (request(path).state == VideoState::Queued)
            ++added;
    }

    if (added > 0) {
        qInfo() << "[Video] Background pass queued" << added << "file(s) under"
                << directory;
    }

    return added;
}

int VideoStore::pending() const
{
    QMutexLocker locker(&m_mutex);
    return m_queue.size() + (m_runningKey.isEmpty() ? 0 : 1);
}

void VideoStore::cancelAll()
{
    QMutexLocker locker(&m_mutex);
    m_queue.clear();
    m_queuedKeys.clear();
    m_cancelRunning = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  The worker
// ─────────────────────────────────────────────────────────────────────────────

void VideoStore::work()
{
    for (;;) {
        Job job;

        {
            QMutexLocker locker(&m_mutex);

            while (m_queue.isEmpty() && !m_stopping)
                m_wakeUp.wait(&m_mutex);

            if (m_stopping)
                return;

            job = m_queue.dequeue();
            m_queuedKeys.remove(job.key);

            m_runningKey     = job.key;
            m_runningSource  = job.sourcePath;
            m_runningPercent = 0;
            m_cancelRunning  = false;
        }

        process(job);

        {
            QMutexLocker locker(&m_mutex);
            m_runningKey.clear();
            m_runningSource.clear();
            m_runningPercent = 0;
        }
    }
}

void VideoStore::process(const Job& job)
{
    const QString name = QFileInfo(job.sourcePath).fileName();

    // ── Probe ─────────────────────────────────────────────────────────────
    const auto profile = m_transcoder->probe(job.sourcePath);
    if (!profile) {
        qWarning() << "[Video] Could not probe" << name << "— not retrying.";
        recordState(job.key, job, QStringLiteral("failed"),
                    QStringLiteral("The file could not be read."), 0, 0);
        emit finished(job.sourcePath, false);
        return;
    }

    // ── Assess ────────────────────────────────────────────────────────────
    QString reason;
    const TranscodeNeed need = VideoTranscoder::assess(*profile, &reason);

    if (need != TranscodeNeed::Recommended) {
        // Recorded, not just returned. Probing is a subprocess, so without this every
        // visit to a folder of videos would spawn one per file for ever.
        qInfo() << "[Video]" << name << "needs no rendition —" << reason;
        recordState(job.key, job,
                    need == TranscodeNeed::NotNeeded ? QStringLiteral("skipped")
                                                     : QStringLiteral("failed"),
                    reason, 0, profile->durationMs);
        emit finished(job.sourcePath, need == TranscodeNeed::NotNeeded);
        return;
    }

    // ── Transcode ─────────────────────────────────────────────────────────
    const QString destination = renditionPath(job.key);

    qInfo() << "[Video] Transcoding" << name << "—" << reason
            << "— expect roughly"
            << qRound(profile->durationMs / 1000.0 * 4.3) << "seconds";

    QString error;
    const bool ok = m_transcoder->transcode(
        job.sourcePath, destination, *profile,
        [this, &job](int percent) {
            QMutexLocker locker(&m_mutex);
            if (m_cancelRunning || m_stopping)
                return false;
            m_runningPercent = percent;
            locker.unlock();

            emit progress(job.sourcePath, percent);
            return true;
        },
        &error);

    if (!ok) {
        bool cancelled = false;
        {
            QMutexLocker locker(&m_mutex);
            cancelled = m_cancelRunning || m_stopping;
        }

        if (cancelled) {
            // No record: a cancellation says nothing about whether the file could be
            // transcoded, and writing `failed` would mean it was never tried again.
            qInfo() << "[Video] Cancelled" << name;
            return;
        }

        qWarning() << "[Video] Failed" << name << ":" << error;
        recordState(job.key, job, QStringLiteral("failed"), error, 0,
                    profile->durationMs);
        emit finished(job.sourcePath, false);
        return;
    }

    const qint64 bytes = QFileInfo(destination).size();

    recordState(job.key, job, QStringLiteral("ready"), reason, bytes,
                profile->durationMs);

    qInfo() << "[Video] Ready" << name << "—"
            << (bytes / (1024.0 * 1024.0)) << "MB from"
            << (job.sourceSize / (1024.0 * 1024.0)) << "MB";

    evictIfNeeded();
    emit finished(job.sourcePath, true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Index and eviction
// ─────────────────────────────────────────────────────────────────────────────

void VideoStore::recordState(const QByteArray& key, const Job& job,
                             const QString& state, const QString& reason,
                             qint64 bytes, qint64 durationMs)
{
    {
        QMutexLocker locker(&m_mutex);
        m_totalBytes += bytes;
    }

    if (!m_database || !m_database->isOpen())
        return;

    const qint64 now = QDateTime::currentSecsSinceEpoch();

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO videos "
        "(key, src_path, mtime_ms, size, state, reason, bytes, duration_ms, "
        " created_at, last_used_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

    query.addBindValue(QString::fromLatin1(key));
    query.addBindValue(job.sourcePath);
    query.addBindValue(job.modifiedMsecs);
    query.addBindValue(job.sourceSize);
    query.addBindValue(state);
    query.addBindValue(reason);
    query.addBindValue(bytes);
    query.addBindValue(durationMs);
    query.addBindValue(now);
    query.addBindValue(now);

    query.exec();
}

void VideoStore::touchIndex(const QByteArray& key)
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "UPDATE videos SET last_used_at = ? WHERE key = ?"));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(QString::fromLatin1(key));
    query.exec();
}

void VideoStore::loadTotalBytesFromIndex()
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (!query.exec(QStringLiteral("SELECT COALESCE(SUM(bytes), 0) FROM videos"))
        || !query.next()) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    m_totalBytes = query.value(0).toLongLong();
}

void VideoStore::setMaxBytes(qint64 bytes)
{
    {
        QMutexLocker locker(&m_mutex);
        m_maxBytes = qMax<qint64>(0, bytes);
    }
    evictIfNeeded();
}

qint64 VideoStore::maxBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_maxBytes;
}

qint64 VideoStore::totalBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_totalBytes;
}

void VideoStore::evictIfNeeded()
{
    if (!m_database || !m_database->isOpen())
        return;

    qint64 ceiling = 0;
    qint64 current = 0;
    {
        QMutexLocker locker(&m_mutex);
        ceiling = m_maxBytes;
        current = m_totalBytes;
    }

    if (current <= ceiling)
        return;

    // Down to 80% rather than exactly to the ceiling, so one more rendition does not
    // immediately trigger another eviction pass.
    const qint64 target = ceiling * 8 / 10;

    QList<QPair<QString, qint64>> victims;
    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        if (!query.exec(QStringLiteral(
                "SELECT key, bytes FROM videos WHERE state = 'ready' "
                "ORDER BY last_used_at ASC"))) {
            return;
        }

        while (query.next() && current > target) {
            const qint64 bytes = query.value(1).toLongLong();
            victims.append({ query.value(0).toString(), bytes });
            current -= bytes;
        }
    }

    for (const auto& [key, bytes] : victims) {
        QFile::remove(renditionPath(key.toLatin1()));

        {
            auto handle = m_database->lock();
            QSqlQuery query(handle.connection());
            query.prepare(QStringLiteral("DELETE FROM videos WHERE key = ?"));
            query.addBindValue(key);
            query.exec();
        }

        QMutexLocker locker(&m_mutex);
        m_totalBytes -= bytes;
    }

    if (!victims.isEmpty()) {
        qInfo() << "[Video] Evicted" << victims.size()
                << "rendition(s) to stay under the cache ceiling.";
    }
}

} // namespace media
