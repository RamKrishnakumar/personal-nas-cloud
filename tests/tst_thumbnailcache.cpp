#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include "db/Database.h"
#include "media/FrameExtractor.h"
#include "media/ThumbnailCache.h"

using media::FrameExtractor;
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

    // ── Remembering what cannot be decoded ───────────────────────────────
    void remembersAFileWhoseExtensionLies();
    void retriesOnceTheFileChanges();
    void remembersFailuresAcrossRestarts();

    // ── Videos and HEIC, via the frame extractor ─────────────────────────
    void offersNothingForAVideoWithoutAnExtractor();
    void offersThumbnailsForVideoAndHeicWithOne();
    void generatesAThumbnailForAVideo();
    void thumbnailsAFileWhoseExtensionLies();
    void theIntermediateStillIsNotLeftBehind();
    void aFileTheExtractorCannotReadIsRemembered();
    void theCapabilityFingerprintChangesWithAnExtractor();

    // ── Eviction ─────────────────────────────────────────────────────────
    void evictionKeepsTheCacheUnderItsCeiling();

private:
    QString sourcePath(const QString& name) const
    {
        return m_tmp->path() + QLatin1Char('/') + name;
    }

    /** A file with the right extension and arbitrary contents. */
    bool writePlaceholder(const QString& name) const
    {
        QFile file(sourcePath(name));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        // Non-trivial length, so the cache key is not derived from a zero size.
        return file.write(QByteArray(4096, 'm')) == 4096;
    }

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<db::Database>  m_database;
    std::unique_ptr<ThumbnailCache> m_cache;
    std::unique_ptr<FrameExtractor> m_frames;
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
    m_frames.reset();        // Held by the cache above.
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
//  Remembering what cannot be decoded
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/**
 * Writes a QuickTime movie header under a .jpg name.
 *
 * Not a contrived case: an iPhone backup folder observed in testing held several
 * six-megabyte `.JPG` files that were really MOVs, plus XML sidecars with the same
 * extension. Every folder visit re-read them in full only to fail.
 */
bool writeLyingJpeg(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    // Explicit length: the header contains NUL bytes, which would truncate a
    // string literal taken at face value.
    file.write(QByteArray("\x00\x00\x00\x14" "ftypqt  " "\x00\x00\x00\x00", 16));
    file.write(QByteArray(4096, '\x11'));      // Body, so it is not trivially small.
    return true;
}

} // namespace

void TestThumbnailCache::remembersAFileWhoseExtensionLies()
{
    const QString source = sourcePath(QStringLiteral("IMG_4972.jpg"));
    QVERIFY(writeLyingJpeg(source));

    QVERIFY2(ThumbnailCache::isSupportedImage(source),
             "The extension must look supported, or this tests nothing");
    QVERIFY(!m_cache->isKnownUndecodable(source));

    bool done = false;
    bool ok   = true;
    m_cache->requestAsync(source, ThumbTier::Small, this,
        [&](bool success, const QString&) { done = true; ok = success; });
    QVERIFY(spinUntil([&] { return done; }));
    QVERIFY(!ok);

    // The point of the feature: one attempt per file version, not one per visit.
    QVERIFY2(m_cache->isKnownUndecodable(source),
             "The failure was not remembered, so every folder visit would retry it");

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM thumb_fails")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
}

void TestThumbnailCache::retriesOnceTheFileChanges()
{
    const QString source = sourcePath(QStringLiteral("IMG_4973.jpg"));
    QVERIFY(writeLyingJpeg(source));

    QVERIFY(!m_cache->generateBlocking(source, ThumbTier::Small));
    QVERIFY(m_cache->isKnownUndecodable(source));

    // Replacing the file changes its mtime and size, so the failure key changes
    // too. That is what removes any need for an invalidation step.
    QVERIFY(QFile::remove(source));
    QVERIFY(writeImage(source, 800, 600));

    QVERIFY2(!m_cache->isKnownUndecodable(source),
             "A replaced file must not inherit the old version's failure");
    QVERIFY2(m_cache->generateBlocking(source, ThumbTier::Small),
             "A file that now decodes was not retried");
}

void TestThumbnailCache::remembersFailuresAcrossRestarts()
{
    const QString source = sourcePath(QStringLiteral("IMG_4975.jpg"));
    QVERIFY(writeLyingJpeg(source));
    QVERIFY(!m_cache->generateBlocking(source, ThumbTier::Small));

    // A restart must not undo the lesson, or the first browse after every launch
    // would pay the full cost again.
    ThumbnailCache reopened(m_database.get(), m_tmp->path() + QStringLiteral("/thumbs"), 1);
    QVERIFY2(reopened.isKnownUndecodable(source),
             "Failures were not loaded from the index on construction");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Videos and HEIC, via the frame extractor
//
//  Driven through the fake_ffmpeg fixture rather than the real binary — see
//  tst_frameextractor for why. What these cover is the join between the two: that
//  the cache asks the extractor for the files Qt cannot read, puts the result
//  through its own encoder, and treats a failure the same way it treats any other
//  undecodable file.
// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailCache::offersNothingForAVideoWithoutAnExtractor()
{
    // The state of the application from step 10 to step 16, and still the state on a
    // machine with no ffmpeg. It has to stay a working configuration rather than
    // becoming a broken one.
    QVERIFY(writePlaceholder(QStringLiteral("clip.mp4")));

    // A video needs a decoder nothing in Qt provides, on every platform.
    QVERIFY(!m_cache->canGenerateFor(QStringLiteral("clip.mp4")));

    // HEIC is different, and asserting it is not readable was wrong. macOS ships a
    // HEIF image plugin with Qt, so there `canGenerateFor("photo.heic")` is correctly
    // true with no ffmpeg anywhere — Apple's own decoder handles it. Windows has no
    // such plugin unless the user installs the HEIF extension, which is why this read
    // as a fact about HEIC rather than a fact about the platform.
    //
    // Asked of the same list the cache itself consults, so this tracks whatever the
    // machine can actually do instead of encoding one machine's answer.
    const bool qtReadsHeic =
        QImageReader::supportedImageFormats().contains(QByteArrayLiteral("heic"));

    QCOMPARE(m_cache->canGenerateFor(QStringLiteral("photo.heic")), qtReadsHeic);

    // And nothing is attempted, so no decode-failure row is written either — the file
    // is not undecodable, it is simply not something we offer to decode.
    QVERIFY(!m_cache->generateBlocking(sourcePath(QStringLiteral("clip.mp4")),
                                       ThumbTier::Small));
    QVERIFY(!m_cache->isKnownUndecodable(sourcePath(QStringLiteral("clip.mp4"))));
}

void TestThumbnailCache::offersThumbnailsForVideoAndHeicWithOne()
{
    qputenv("FAKE_FFMPEG_MODE", "ok");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(m_frames->isAvailable());

    m_cache->setFrameExtractor(m_frames.get());

    QVERIFY(m_cache->canGenerateFor(QStringLiteral("clip.mp4")));
    QVERIFY(m_cache->canGenerateFor(QStringLiteral("photo.heic")));

    // Unchanged for everything else: a JPEG must keep going through Qt's reader and
    // its scaled-decode path, not through a subprocess.
    QVERIFY(m_cache->canGenerateFor(QStringLiteral("photo.jpg")));
    QVERIFY(!m_cache->canGenerateFor(QStringLiteral("notes.txt")));
}

void TestThumbnailCache::generatesAThumbnailForAVideo()
{
    qputenv("FAKE_FFMPEG_MODE", "ok");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    m_cache->setFrameExtractor(m_frames.get());

    QVERIFY(writePlaceholder(QStringLiteral("clip.mp4")));
    const QString source = sourcePath(QStringLiteral("clip.mp4"));

    QVERIFY(m_cache->generateBlocking(source, ThumbTier::Small));

    const auto cached = m_cache->hit(source, ThumbTier::Small);
    QVERIFY(cached.has_value());

    // In the cache's own format, not ffmpeg's. The still is an intermediate; the
    // thumbnail that gets served has to come out of the same encoder at the same
    // quality as every other thumbnail, or the format and the size budget in §5.4
    // would depend on which decoder happened to read the source.
    const QImage thumbnail(*cached);
    QVERIFY2(!thumbnail.isNull(), qPrintable(*cached));
    QVERIFY(thumbnail.width() <= media::tierLongEdge(ThumbTier::Small));
    QVERIFY(thumbnail.height() <= media::tierLongEdge(ThumbTier::Small));

    QCOMPARE(QFileInfo(*cached).suffix().toUtf8(), m_cache->outputFormat());
}

void TestThumbnailCache::thumbnailsAFileWhoseExtensionLies()
{
    // The other half of remembersAFileWhoseExtensionLies(). That one asserts the
    // failure is remembered when nothing can read the file; this one asserts that
    // with ffmpeg present it is not a failure at all.
    //
    // Qt dispatches on the extension, so a `.JPG` holding a QuickTime movie goes to
    // the JPEG reader and fails. Extension-driven dispatch is exactly what hid these
    // files, so the fallback cannot be gated on the extension either — it has to run
    // whenever Qt could not read something it claimed it could.
    qputenv("FAKE_FFMPEG_MODE", "ok");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(m_frames->isAvailable());
    m_cache->setFrameExtractor(m_frames.get());

    const QString source = sourcePath(QStringLiteral("IMG_4976.jpg"));
    QVERIFY(writeLyingJpeg(source));

    QVERIFY2(ThumbnailCache::isSupportedImage(source),
             "the extension must look supported, or this tests nothing");

    QVERIFY2(m_cache->generateBlocking(source, ThumbTier::Small),
             "a mislabelled video got no thumbnail despite ffmpeg being available");

    QVERIFY2(!m_cache->isKnownUndecodable(source),
             "a file that did decode must not be remembered as undecodable");

    const auto cached = m_cache->hit(source, ThumbTier::Small);
    QVERIFY(cached.has_value());
    QVERIFY2(!QImage(*cached).isNull(), qPrintable(*cached));

    // The fallback writes and removes its own intermediate, so it has its own chance
    // to leak one — see theIntermediateStillIsNotLeftBehind() for the other path.
    QDirIterator leftovers(m_tmp->path() + QStringLiteral("/thumbs"),
                           { QStringLiteral("*.still.png") },
                           QDir::Files, QDirIterator::Subdirectories);
    QVERIFY2(!leftovers.hasNext(),
             qPrintable(QStringLiteral("intermediate left behind: %1")
                            .arg(leftovers.next())));
}

void TestThumbnailCache::theIntermediateStillIsNotLeftBehind()
{
    qputenv("FAKE_FFMPEG_MODE", "ok");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    m_cache->setFrameExtractor(m_frames.get());

    QVERIFY(writePlaceholder(QStringLiteral("clip.mp4")));
    QVERIFY(m_cache->generateBlocking(sourcePath(QStringLiteral("clip.mp4")),
                                      ThumbTier::Small));

    // The intermediate PNG is written into the cache directory, so if it were left
    // behind it would count towards the size ceiling and be evicted as though it were
    // a thumbnail — while never being served to anybody.
    QDirIterator leftovers(m_tmp->path() + QStringLiteral("/thumbs"),
                           { QStringLiteral("*.still.png") },
                           QDir::Files, QDirIterator::Subdirectories);

    QVERIFY2(!leftovers.hasNext(),
             qPrintable(QStringLiteral("intermediate left behind: %1")
                            .arg(leftovers.next())));
}

void TestThumbnailCache::aFileTheExtractorCannotReadIsRemembered()
{
    // ffmpeg exiting 0 without writing anything — which it really does — must land in
    // the same negative cache as a `.JPG` that is really a movie. Without that, every
    // visit to a folder of unsupported clips spawns a process per file, for ever.
    qputenv("FAKE_FFMPEG_MODE", "silent_success");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(m_frames->isAvailable());
    m_cache->setFrameExtractor(m_frames.get());

    QVERIFY(writePlaceholder(QStringLiteral("broken.mp4")));
    const QString source = sourcePath(QStringLiteral("broken.mp4"));

    QVERIFY(!m_cache->generateBlocking(source, ThumbTier::Small));
    QVERIFY2(m_cache->isKnownUndecodable(source),
             "a file ffmpeg could not produce a frame from must not be retried");

    // Restored, so a later test in this process is not affected by the mode.
    qputenv("FAKE_FFMPEG_MODE", "ok");
}

void TestThumbnailCache::theCapabilityFingerprintChangesWithAnExtractor()
{
    // What the background scanner stores next to each swept directory, so it can tell
    // that it can now read more than it could when it last looked. If this did not
    // change, a library already scanned would never be warmed for the new formats.
    const QByteArray withoutFfmpeg = m_cache->capabilityFingerprint();
    QVERIFY(!withoutFfmpeg.isEmpty());

    // Stable across calls, or every launch would re-sweep the whole library.
    QCOMPARE(m_cache->capabilityFingerprint(), withoutFfmpeg);

    qputenv("FAKE_FFMPEG_MODE", "ok");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(m_frames->isAvailable());
    m_cache->setFrameExtractor(m_frames.get());

    QVERIFY2(m_cache->capabilityFingerprint() != withoutFfmpeg,
             "gaining ffmpeg must change the fingerprint, or nothing is re-swept");
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
