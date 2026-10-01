#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include "db/Database.h"
#include "media/ThumbnailCache.h"

using media::ThumbnailCache;
using media::ThumbTier;

namespace {

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 10'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

/** Writes a plain image of the given size, large enough to need downscaling. */
bool writeImage(const QString& path, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    // A gradient rather than a flat fill: flat colour compresses to almost nothing
    // and would make size assertions meaningless.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb((x * 7) % 256, (y * 11) % 256, (x ^ y) % 256));
    return image.save(path);
}

} // namespace

/**
 * Tests for media::ThumbnailCache.
 *
 * The behaviours worth guarding are the ones that silently degrade: a key that
 * does not change when a file is edited would serve a stale image forever under
 * the immutable cache policy, and a failure to coalesce would decode the same
 * photo once per browser tab.
 */
class TestThumbnailCache : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ── Keys ─────────────────────────────────────────────────────────────
    void keyIsStableForTheSameInputs();
    void keyChangesWhenTheFileChanges();
    void keyDiffersPerTier();
    void keyIgnoresSeparatorAndCaseDifferences();
    void versionTagIsAPrefixOfTheKey();
    void tierCodesRoundTrip();

    // ── Generation ───────────────────────────────────────────────────────
    void generatesAndCachesAThumbnail();
    void thumbnailIsSmallerThanTheOriginal();
    void mediumTierIsLargerThanSmall();
    void doesNotUpscaleASmallImage();
    void secondRequestIsAHitWithoutGenerating();
    void concurrentRequestsForOneKeyAreCoalesced();
    void failureIsReportedForANonImage();

    // ── Eviction ─────────────────────────────────────────────────────────
    void evictionKeepsTheCacheUnderItsCeiling();

private:
    QString sourcePath(const QString& name) const
    {
        return m_tmp->path() + QLatin1Char('/') + name;
    }

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<db::Database>  m_database;
    std::unique_ptr<ThumbnailCache> m_cache;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailCache::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp->path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_cache = std::make_unique<ThumbnailCache>(
        m_database.get(), m_tmp->path() + QStringLiteral("/thumbs"), 2);
}

void TestThumbnailCache::cleanup()
{
    m_cache.reset();
    m_database.reset();
    m_tmp.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Keys
// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailCache::keyIsStableForTheSameInputs()
{
    const QByteArray first  = media::thumbnailKey("/a/b.jpg", 1000, 500, ThumbTier::Small);
    const QByteArray second = media::thumbnailKey("/a/b.jpg", 1000, 500, ThumbTier::Small);

    QCOMPARE(first, second);
    QVERIFY(!first.isEmpty());
}

void TestThumbnailCache::keyChangesWhenTheFileChanges()
{
    const QByteArray base = media::thumbnailKey("/a/b.jpg", 1000, 500, ThumbTier::Small);

    // Both inputs must affect the key. If they did not, an edited photo would keep
    // its old URL and the immutable cache policy would serve the stale thumbnail
    // for a year.
    QVERIFY(media::thumbnailKey("/a/b.jpg", 2000, 500, ThumbTier::Small) != base);
    QVERIFY(media::thumbnailKey("/a/b.jpg", 1000, 501, ThumbTier::Small) != base);
    QVERIFY(media::thumbnailKey("/a/c.jpg", 1000, 500, ThumbTier::Small) != base);
}

void TestThumbnailCache::keyDiffersPerTier()
{
    QVERIFY(media::thumbnailKey("/a/b.jpg", 1, 2, ThumbTier::Small)
            != media::thumbnailKey("/a/b.jpg", 1, 2, ThumbTier::Medium));
}

void TestThumbnailCache::keyIgnoresSeparatorAndCaseDifferences()
{
    // The same file reached by a differently spelled path must not be decoded and
    // stored twice.
    const QByteArray canonical = media::thumbnailKey("/a/b/c.jpg", 1, 2, ThumbTier::Small);
    QCOMPARE(media::thumbnailKey("/a//b/c.jpg", 1, 2, ThumbTier::Small), canonical);
    QCOMPARE(media::thumbnailKey("/a/./b/c.jpg", 1, 2, ThumbTier::Small), canonical);
    QCOMPARE(media::thumbnailKey("/A/B/C.JPG", 1, 2, ThumbTier::Small), canonical);
}

void TestThumbnailCache::versionTagIsAPrefixOfTheKey()
{
    const QByteArray key = media::thumbnailKey("/a/b.jpg", 1, 2, ThumbTier::Small);
    const QByteArray tag = media::thumbnailVersionTag("/a/b.jpg", 1, 2, ThumbTier::Small);

    QVERIFY(!tag.isEmpty());
    QVERIFY(key.startsWith(tag));

    // It must still change with the file, since that is its only purpose.
    QVERIFY(media::thumbnailVersionTag("/a/b.jpg", 9, 2, ThumbTier::Small) != tag);
}

void TestThumbnailCache::tierCodesRoundTrip()
{
    QCOMPARE(media::tierFromCode(media::tierCode(ThumbTier::Small)), ThumbTier::Small);
    QCOMPARE(media::tierFromCode(media::tierCode(ThumbTier::Medium)), ThumbTier::Medium);
    QVERIFY(!media::tierFromCode('x').has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Generation
// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailCache::generatesAndCachesAThumbnail()
{
    const QString source = sourcePath(QStringLiteral("photo.png"));
    QVERIFY(writeImage(source, 2000, 1500));

    QVERIFY2(!m_cache->hit(source, ThumbTier::Small).has_value(),
             "A cold cache must report a miss");

    bool    done = false;
    bool    ok   = false;
    QString produced;

    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool success, const QString& path) {
            done = true; ok = success; produced = path;
        });

    QVERIFY(spinUntil([&] { return done; }));
    QVERIFY2(ok, "Generation failed");
    QVERIFY(QFile::exists(produced));

    // Long edge honoured, aspect ratio preserved.
    const QImage thumb(produced);
    QVERIFY(!thumb.isNull());
    QCOMPARE(qMax(thumb.width(), thumb.height()), media::tierLongEdge(ThumbTier::Small));
    QVERIFY(qAbs(double(thumb.width()) / thumb.height() - 2000.0 / 1500.0) < 0.05);
}

void TestThumbnailCache::thumbnailIsSmallerThanTheOriginal()
{
    // The entire point of the feature. Asserted on pixel count rather than on a
    // byte ratio: how many bytes an image costs depends entirely on its content,
    // and a synthetic test pattern is not representative either way — a smooth
    // gradient compresses superbly as PNG while its JPEG thumbnail does not.
    // Pixels are what the feature actually controls.
    const QString source = sourcePath(QStringLiteral("big.png"));
    QVERIFY(writeImage(source, 3000, 2000));

    bool done = false;
    QString produced;
    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool ok, const QString& path) { done = true; if (ok) produced = path; });

    QVERIFY(spinUntil([&] { return done; }));
    QVERIFY(!produced.isEmpty());

    const QImage thumb(produced);
    QVERIFY(!thumb.isNull());

    const qint64 originalPixels = 3000LL * 2000;
    const qint64 thumbPixels    = qint64(thumb.width()) * thumb.height();

    QVERIFY2(thumbPixels * 20 < originalPixels,
             qPrintable(QStringLiteral("thumbnail %1 pixels vs original %2")
                            .arg(thumbPixels).arg(originalPixels)));
}

void TestThumbnailCache::mediumTierIsLargerThanSmall()
{
    const QString source = sourcePath(QStringLiteral("both.png"));
    QVERIFY(writeImage(source, 3000, 2000));

    QString smallPath, mediumPath;
    int finished = 0;

    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool ok, const QString& p) { ++finished; if (ok) smallPath = p; });
    m_cache->requestAsync(source, ThumbTier::Medium, this,
        [&](bool ok, const QString& p) { ++finished; if (ok) mediumPath = p; });

    QVERIFY(spinUntil([&] { return finished == 2; }));
    QVERIFY(!smallPath.isEmpty());
    QVERIFY(!mediumPath.isEmpty());
    QVERIFY2(smallPath != mediumPath, "The two tiers must not share one cache file");

    QCOMPARE(qMax(QImage(mediumPath).width(), QImage(mediumPath).height()),
             media::tierLongEdge(ThumbTier::Medium));
}

void TestThumbnailCache::doesNotUpscaleASmallImage()
{
    // Enlarging a small image would waste bytes and add nothing.
    const QString source = sourcePath(QStringLiteral("tiny.png"));
    QVERIFY(writeImage(source, 120, 90));

    bool done = false;
    QString produced;
    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool ok, const QString& p) { done = true; if (ok) produced = p; });

    QVERIFY(spinUntil([&] { return done; }));
    QVERIFY(!produced.isEmpty());

    const QImage thumb(produced);
    QCOMPARE(thumb.width(), 120);
    QCOMPARE(thumb.height(), 90);
}

void TestThumbnailCache::secondRequestIsAHitWithoutGenerating()
{
    const QString source = sourcePath(QStringLiteral("again.png"));
    QVERIFY(writeImage(source, 1200, 900));

    bool done = false;
    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool, const QString&) { done = true; });
    QVERIFY(spinUntil([&] { return done; }));

    const auto cached = m_cache->hit(source, ThumbTier::Small);
    QVERIFY2(cached.has_value(), "A generated thumbnail must be reported as a hit");

    // A hit resolves synchronously, which is what keeps a warm folder cheap.
    bool immediate = false;
    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool, const QString&) { immediate = true; });
    QVERIFY2(immediate, "A cache hit must call back without waiting for the pool");
}

void TestThumbnailCache::concurrentRequestsForOneKeyAreCoalesced()
{
    // Two browser tabs on one folder must not decode the same photo twice.
    const QString source = sourcePath(QStringLiteral("shared.png"));
    QVERIFY(writeImage(source, 2400, 1800));

    int finished = 0;
    QStringList produced;

    for (int i = 0; i < 6; ++i) {
        m_cache->requestAsync(source, ThumbTier::Small, this,
            [&](bool ok, const QString& path) {
                ++finished;
                if (ok)
                    produced << path;
            });
    }

    QVERIFY(spinUntil([&] { return finished == 6; }));
    QCOMPARE(produced.size(), 6);

    // Every waiter gets the same file, and the index holds one row for it.
    QCOMPARE(QSet<QString>(produced.begin(), produced.end()).size(), 1);

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM thumbs")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
}

void TestThumbnailCache::failureIsReportedForANonImage()
{
    const QString source = sourcePath(QStringLiteral("notes.txt"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not an image");
    file.close();

    bool done = false;
    bool ok   = true;
    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool success, const QString&) { done = true; ok = success; });

    QVERIFY(spinUntil([&] { return done; }));
    QVERIFY2(!ok, "A non-image must report failure rather than hang");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Eviction
// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailCache::evictionKeepsTheCacheUnderItsCeiling()
{
    int finished = 0;
    for (int i = 0; i < 8; ++i) {
        const QString source = sourcePath(QStringLiteral("evict-%1.png").arg(i));
        QVERIFY(writeImage(source, 1600, 1200));
        m_cache->requestAsync(source, ThumbTier::Small, this,
            [&](bool, const QString&) { ++finished; });
    }
    QVERIFY(spinUntil([&] { return finished == 8; }));

    const qint64 grown = m_cache->totalBytes();
    QVERIFY2(grown > 0, "Nothing was recorded, so eviction cannot be tested");

    // Squeeze the ceiling well below what is stored.
    m_cache->setMaxBytes(grown / 4);

    QVERIFY2(m_cache->totalBytes() <= grown / 4,
             qPrintable(QStringLiteral("cache holds %1 bytes with a ceiling of %2")
                            .arg(m_cache->totalBytes()).arg(grown / 4)));

    // Evicted rows must go with their files, or the accounting drifts.
    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral("SELECT COALESCE(SUM(bytes),0) FROM thumbs")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toLongLong(), m_cache->totalBytes());
}

QTEST_MAIN(TestThumbnailCache)
#include "tst_thumbnailcache.moc"
