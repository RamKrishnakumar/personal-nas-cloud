#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include "db/Database.h"
#include "media/FrameExtractor.h"
#include "media/ThumbnailCache.h"
#include "media/ThumbnailScanner.h"

using media::FrameExtractor;
using media::ThumbnailCache;
using media::ThumbnailScanner;
using media::ThumbTier;

namespace {

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 20'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return true;
}

bool writeImage(const QString& path, int width = 600, int height = 400)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb((x * 3) % 256, (y * 7) % 256, (x ^ y) % 256));
    return image.save(path);
}

} // namespace

/**
 * Tests for media::ThumbnailScanner.
 *
 * Two behaviours carry the design and are covered first: yielding while a transfer
 * is active, and stopping rather than thrashing once the cache is full. A warm-up
 * pass that stutters someone's video, or that evicts its own results and starts
 * again, is worse than having no warm-up at all.
 */
class TestThumbnailScanner : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void warmsTheCacheForAWholeTree();
    void skipsFilesItCannotDecode();
    void yieldsWhileTheServerIsBusy();
    void resumesWorkOnceTheServerIsIdle();
    void stopsWhenTheCacheIsFull();
    void recordsProgressSoARestartResumes();
    void rescansADirectoryThatChanged();
    void rescansEverythingWhenItLearnsToReadANewFormat();
    void stopIsHonoured();
    void handlesAMissingRoot();
    void doesNotFollowDirectorySymlinks();

private:
    QString path(const QString& relative) const
    {
        return m_tmp->path() + QLatin1Char('/') + relative;
    }

    std::unique_ptr<QTemporaryDir>   m_tmp;
    std::unique_ptr<db::Database>    m_database;
    std::unique_ptr<FrameExtractor>  m_frames;
    std::unique_ptr<ThumbnailCache>  m_cache;
    std::unique_ptr<ThumbnailScanner> m_scanner;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailScanner::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());

    QVERIFY(QDir().mkpath(path(QStringLiteral("nas/Album/2024"))));
    QVERIFY(QDir().mkpath(path(QStringLiteral("nas/Album/2025"))));

    for (int i = 0; i < 3; ++i) {
        QVERIFY(writeImage(path(QStringLiteral("nas/Album/2024/a%1.png").arg(i))));
        QVERIFY(writeImage(path(QStringLiteral("nas/Album/2025/b%1.png").arg(i))));
    }
    QVERIFY(writeImage(path(QStringLiteral("nas/top.png"))));

    m_database = std::make_unique<db::Database>();
    QVERIFY(m_database->open(path(QStringLiteral("index.sqlite"))));

    m_cache = std::make_unique<ThumbnailCache>(
        m_database.get(), path(QStringLiteral("thumbs")), 1);

    m_scanner = std::make_unique<ThumbnailScanner>(m_cache.get(), m_database.get());
    m_scanner->setRoot(path(QStringLiteral("nas")));
    m_scanner->setPauseOnBattery(false);   // Must not depend on the test machine.
}

void TestThumbnailScanner::cleanup()
{
    m_scanner.reset();
    m_cache.reset();
    m_frames.reset();        // Held by the cache above.
    m_database.reset();
    m_tmp.reset();
}

// ─────────────────────────────────────────────────────────────────────────────

void TestThumbnailScanner::warmsTheCacheForAWholeTree()
{
    m_scanner->start();
    QVERIFY2(spinUntil([this] { return !m_scanner->isRunning(); }),
             "The scan did not finish");

    const auto progress = m_scanner->progress();
    QCOMPARE(progress.thumbnailsCreated, 7);

    // Nested directories must be reached, not only the root.
    QVERIFY(m_cache->hit(path(QStringLiteral("nas/top.png")), ThumbTier::Small));
    QVERIFY(m_cache->hit(path(QStringLiteral("nas/Album/2024/a0.png")), ThumbTier::Small));
    QVERIFY(m_cache->hit(path(QStringLiteral("nas/Album/2025/b2.png")), ThumbTier::Small));
}

void TestThumbnailScanner::skipsFilesItCannotDecode()
{
    QFile notes(path(QStringLiteral("nas/notes.txt")));
    QVERIFY(notes.open(QIODevice::WriteOnly));
    notes.write("not an image");
    notes.close();

    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));

    // Undecodable files must not even be examined, or a library of HEIC photos
    // would spend the whole pass failing to decode them.
    QCOMPARE(m_scanner->progress().filesExamined, 7);
}

void TestThumbnailScanner::yieldsWhileTheServerIsBusy()
{
    // The behaviour that makes this feature safe to ship. While a transfer is
    // active the scanner must do no work at all.
    bool busy = true;
    m_scanner->setBusyPredicate([&busy] { return busy; });

    m_scanner->start();

    // Give it well over its working interval to prove it is not just slow.
    QVERIFY(spinUntil([this] { return m_scanner->progress().waiting; }, 5000));

    const int createdWhileBusy = m_scanner->progress().thumbnailsCreated;
    QThread::msleep(500);
    QCoreApplication::processEvents();

    QCOMPARE(m_scanner->progress().thumbnailsCreated, createdWhileBusy);
    QVERIFY2(createdWhileBusy == 0,
             "The scanner generated thumbnails while a transfer was active");
    QVERIFY(m_scanner->isRunning());      // Waiting, not finished.

    busy = false;
    m_scanner->stop();
}

void TestThumbnailScanner::resumesWorkOnceTheServerIsIdle()
{
    bool busy = true;
    m_scanner->setBusyPredicate([&busy] { return busy; });

    m_scanner->start();
    QVERIFY(spinUntil([this] { return m_scanner->progress().waiting; }, 5000));
    QCOMPARE(m_scanner->progress().thumbnailsCreated, 0);

    // Yielding must be a pause, not an abandonment.
    busy = false;
    QVERIFY2(spinUntil([this] { return !m_scanner->isRunning(); }),
             "The scanner did not resume after the server went idle");
    QCOMPARE(m_scanner->progress().thumbnailsCreated, 7);
}

void TestThumbnailScanner::stopsWhenTheCacheIsFull()
{
    // A library larger than the cache would otherwise scan, evict its own earliest
    // results, and scan them again indefinitely.
    m_cache->setMaxBytes(1);     // Already over capacity by any measure.

    m_scanner->start();
    QVERIFY2(spinUntil([this] { return !m_scanner->isRunning(); }, 10'000),
             "The scanner did not stop with a full cache");

    QVERIFY2(m_scanner->progress().thumbnailsCreated <= 1,
             "The scanner kept generating into a full cache");
}

void TestThumbnailScanner::recordsProgressSoARestartResumes()
{
    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));

    // Four directories recorded: the root, Album, and the two year folders.
    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM scan_state")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 4);
}

void TestThumbnailScanner::rescansADirectoryThatChanged()
{
    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));
    QCOMPARE(m_scanner->progress().filesExamined, 7);

    // A second pass over an unchanged tree should examine nothing, which is what
    // makes running on every launch cheap.
    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));
    QCOMPARE(m_scanner->progress().filesExamined, 0);

    // A new photo changes the directory's modification time, so it is swept again.
    // The recorded timestamp has one-second resolution, so wait past it rather than
    // racing the clock.
    QThread::msleep(1100);
    QVERIFY(writeImage(path(QStringLiteral("nas/Album/2024/new.png"))));

    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));
    QVERIFY2(m_scanner->progress().filesExamined > 0,
             "A changed directory was not rescanned");
    QVERIFY(m_cache->hit(path(QStringLiteral("nas/Album/2024/new.png")), ThumbTier::Small));
}

void TestThumbnailScanner::rescansEverythingWhenItLearnsToReadANewFormat()
{
    // The step 17 bug, reproduced. A library already fully swept stays marked as
    // finished, so when the application learns to read a new format nothing warms it
    // and every one of those files is generated on first view instead — a subprocess
    // at a time, while somebody waits for the folder to paint.
    //
    // Observed in the field as "142 directories, 0 files examined" on a machine where
    // ffmpeg had just been installed.
    {
        QFile clip(path(QStringLiteral("nas/Album/2024/clip.mp4")));
        QVERIFY(clip.open(QIODevice::WriteOnly));
        clip.write(QByteArray(2048, 'v'));
    }

    // Pass one, with no extractor: the clip is not something we can read yet.
    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));
    QCOMPARE(m_scanner->progress().filesExamined, 7);
    QVERIFY(!m_cache->hit(path(QStringLiteral("nas/Album/2024/clip.mp4")),
                          ThumbTier::Small).has_value());

    // Pass two changes nothing, which is the behaviour that makes running on every
    // launch cheap — and also what hid the bug.
    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));
    QCOMPARE(m_scanner->progress().filesExamined, 0);

    // Now ffmpeg appears. Nothing on disk changed, so every directory modification
    // time is older than its sweep — the only thing that differs is what we can read.
    qputenv("FAKE_FFMPEG_MODE", "ok");
    m_frames = std::make_unique<FrameExtractor>(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(m_frames->isAvailable());
    m_cache->setFrameExtractor(m_frames.get());

    m_scanner->start();
    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }));

    QVERIFY2(m_scanner->progress().filesExamined > 0,
             "a library that can now be read further was not swept again");

    QVERIFY2(m_cache->hit(path(QStringLiteral("nas/Album/2024/clip.mp4")),
                          ThumbTier::Small).has_value(),
             "the newly readable video was not warmed");
}

void TestThumbnailScanner::stopIsHonoured()
{
    m_scanner->start();
    m_scanner->stop();

    QVERIFY2(spinUntil([this] { return !m_scanner->isRunning(); }, 5000),
             "stop() was not honoured");
}

void TestThumbnailScanner::handlesAMissingRoot()
{
    // The configured root may not exist — a disconnected drive, or a typo in
    // settings. It must give up quietly rather than spin or crash.
    m_scanner->setRoot(path(QStringLiteral("no-such-directory")));
    m_scanner->start();

    QVERIFY(spinUntil([this] { return !m_scanner->isRunning(); }, 5000));
    QCOMPARE(m_scanner->progress().thumbnailsCreated, 0);
}

void TestThumbnailScanner::doesNotFollowDirectorySymlinks()
{
    // A link pointing at an ancestor would make the walk recurse forever.
    const QString link = path(QStringLiteral("nas/Album/loop"));

#ifdef Q_OS_WIN
    const int created = QProcess::execute(QStringLiteral("cmd"), {
        QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
        QDir::toNativeSeparators(link),
        QDir::toNativeSeparators(path(QStringLiteral("nas")))
    });
    if (created != 0)
        QSKIP("Could not create a directory junction on this account");
#else
    if (!QFile::link(path(QStringLiteral("nas")), link))
        QSKIP("Could not create a symlink on this platform/account");
#endif

    m_scanner->start();
    const bool finished = spinUntil([this] { return !m_scanner->isRunning(); }, 15'000);

#ifdef Q_OS_WIN
    QProcess::execute(QStringLiteral("cmd"), {
        QStringLiteral("/c"), QStringLiteral("rmdir"), QDir::toNativeSeparators(link)
    });
#else
    QFile::remove(link);
#endif

    QVERIFY2(finished, "The walk did not terminate; a link was followed into a cycle");
}

QTEST_MAIN(TestThumbnailScanner)
#include "tst_thumbnailscanner.moc"
