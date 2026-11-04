#include <QTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>

#include "media/FrameExtractor.h"

using media::FrameExtractor;

namespace {

/** Chooses how the stand-in binary behaves for the next call. */
void setFakeMode(const QByteArray& mode)
{
    qputenv("FAKE_FFMPEG_MODE", mode);
}

} // namespace

/**
 * Unit tests for media::FrameExtractor, against a stand-in binary.
 *
 * The real ffmpeg is not used here on purpose. It is ~80 MB, it may not be installed,
 * and what it does with a given file depends on how it was built — so a suite that
 * drove it would be testing somebody else's binary and would say different things on
 * different machines.
 *
 * What this class actually owns is plumbing: the arguments it builds, the output it
 * verifies, and its behaviour when the process lies or wedges. `fake_ffmpeg`
 * misbehaves on demand, which makes all of that deterministic. Whether real ffmpeg
 * produces a *good-looking* frame is a question for manual verification against real
 * files, and the plan's step 17 criterion is written that way.
 */
class TestFrameExtractor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ── Classification ───────────────────────────────────────────────────
    void recognisesTheHeifFamily();
    void recognisesVideoContainers();
    void leavesQtReadableImagesAlone();
    void classificationIgnoresCase();

    // ── Finding the binary ───────────────────────────────────────────────
    void usesTheConfiguredBinary();
    void reportsUnavailableWhenNothingIsFound();
    void rejectsABinaryThatFailsItsVersionProbe();
    void anUnavailableExtractorRefusesToExtract();

    // ── Extracting ───────────────────────────────────────────────────────
    void producesAReadableStillFromAVideo();
    void passesNostdinAndASingleFrame();
    void passesAScaleFilterThatNeverUpscales();
    void seeksPastTheStartForAPosterFrame();
    void doesNotSeekForAStillImage();
    void fallsBackToTheFirstFrameWhenTheClipIsShort();

    // ── When ffmpeg misbehaves ───────────────────────────────────────────
    void treatsExitZeroWithNoOutputAsFailure();
    void treatsAnEmptyOutputFileAsFailure();
    void treatsUnreadableOutputAsFailure();
    void killsAProcessThatNeverFinishes();
    void rejectsANonPositiveLongEdge();

private:
    /** Lines the fixture logged, one per invocation. */
    QStringList invocations() const;

    void clearLog();

    QString sourcePath(const QString& name) const;
    QString destinationPath() const;

    QTemporaryDir m_tmp;
    QString       m_logPath;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestFrameExtractor::initTestCase()
{
    QVERIFY(m_tmp.isValid());

    QVERIFY2(QFileInfo::exists(QStringLiteral(FAKE_FFMPEG_PATH)),
             "the fake_ffmpeg fixture was not built");

    m_logPath = m_tmp.filePath(QStringLiteral("calls.log"));
    qputenv("FAKE_FFMPEG_LOG", m_logPath.toLocal8Bit());

    // The extractor never reads the source, only hands its path to the subprocess, so
    // these only have to exist and have the right extension.
    for (const QString& name : { QStringLiteral("clip.mp4"),
                                 QStringLiteral("photo.heic") }) {
        QFile file(sourcePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not really media");
    }
}

void TestFrameExtractor::init()
{
    setFakeMode("ok");
    clearLog();
    QFile::remove(destinationPath());
}

void TestFrameExtractor::clearLog()
{
    QFile::remove(m_logPath);
}

QStringList TestFrameExtractor::invocations() const
{
    QFile log(m_logPath);
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList lines;
    QTextStream stream(&log);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (!line.isEmpty())
            lines.append(line);
    }
    return lines;
}

QString TestFrameExtractor::sourcePath(const QString& name) const
{
    return m_tmp.filePath(name);
}

QString TestFrameExtractor::destinationPath() const
{
    return m_tmp.filePath(QStringLiteral("out/still.png"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Classification
// ─────────────────────────────────────────────────────────────────────────────

void TestFrameExtractor::recognisesTheHeifFamily()
{
    // The reason this class exists at all: Qt Image Formats has no HEIF plugin, so an
    // iPhone library has no thumbnails whatsoever without it.
    for (const char* name : { "a.heic", "a.heif", "a.hif", "a.avif" }) {
        QVERIFY2(FrameExtractor::isHeifStill(QLatin1String(name)), name);
        QVERIFY2(FrameExtractor::handles(QLatin1String(name)), name);
        QVERIFY2(!FrameExtractor::isVideo(QLatin1String(name)), name);
    }
}

void TestFrameExtractor::recognisesVideoContainers()
{
    for (const char* name : { "a.mp4", "a.mov", "a.mkv", "a.webm", "a.avi", "a.m2ts" }) {
        QVERIFY2(FrameExtractor::isVideo(QLatin1String(name)), name);
        QVERIFY2(FrameExtractor::handles(QLatin1String(name)), name);
        QVERIFY2(!FrameExtractor::isHeifStill(QLatin1String(name)), name);
    }
}

void TestFrameExtractor::leavesQtReadableImagesAlone()
{
    // Claiming these would route every JPEG through a subprocess, throwing away the
    // scaled-decode path that makes thumbnailing a library minutes rather than hours.
    for (const char* name : { "a.jpg", "a.jpeg", "a.png", "a.webp", "a.gif",
                              "a.txt", "a", "a.mp3" }) {
        QVERIFY2(!FrameExtractor::handles(QLatin1String(name)), name);
    }
}

void TestFrameExtractor::classificationIgnoresCase()
{
    // iOS writes .JPG and .HEIC in capitals, and this is exactly where that bit
    // before — see §5.8 of the plan.
    QVERIFY(FrameExtractor::isHeifStill(QStringLiteral("IMG_4972.HEIC")));
    QVERIFY(FrameExtractor::isVideo(QStringLiteral("IMG_4972.MOV")));
    QVERIFY(FrameExtractor::handles(QStringLiteral("CLIP.Mp4")));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Finding the binary
// ─────────────────────────────────────────────────────────────────────────────

void TestFrameExtractor::usesTheConfiguredBinary()
{
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));

    QVERIFY(extractor.isAvailable());
    QCOMPARE(QFileInfo(extractor.executable()).canonicalFilePath(),
             QFileInfo(QStringLiteral(FAKE_FFMPEG_PATH)).canonicalFilePath());

    // Proves the probe ran and its output was captured, rather than the binary being
    // trusted because the file exists.
    QVERIFY2(extractor.versionLine().contains(QStringLiteral("fake")),
             qPrintable(extractor.versionLine()));
}

void TestFrameExtractor::reportsUnavailableWhenNothingIsFound()
{
    // PATH is emptied for the duration, because otherwise this test says different
    // things on a machine that happens to have ffmpeg installed — and a test whose
    // answer depends on the developer's machine is worse than no test.
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", QByteArray());

    const FrameExtractor extractor(m_tmp.filePath(QStringLiteral("no-such-ffmpeg")));

    qputenv("PATH", savedPath);

    QVERIFY2(!extractor.isAvailable(),
             "a configured path that is not a file must not be accepted");
    QVERIFY(extractor.executable().isEmpty());
}

void TestFrameExtractor::rejectsABinaryThatFailsItsVersionProbe()
{
    // A wrong-architecture binary, a broken symlink or a wrapper script with a
    // missing interpreter all pass an existence check and then fail on every single
    // thumbnail. Failing once, here, is the difference between one log line and
    // thousands.
    setFakeMode("bad_version");

    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", QByteArray());

    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));

    qputenv("PATH", savedPath);

    QVERIFY(!extractor.isAvailable());
}

void TestFrameExtractor::anUnavailableExtractorRefusesToExtract()
{
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", QByteArray());

    const FrameExtractor extractor(m_tmp.filePath(QStringLiteral("no-such-ffmpeg")));

    qputenv("PATH", savedPath);

    QString error;
    QVERIFY(!extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                    destinationPath(), 320, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(destinationPath()));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Extracting
// ─────────────────────────────────────────────────────────────────────────────

void TestFrameExtractor::producesAReadableStillFromAVideo()
{
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(extractor.isAvailable());

    QString error;
    QVERIFY2(extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                    destinationPath(), 320, &error),
             qPrintable(error));

    // The directory did not exist beforehand, so this also covers creating it — a
    // thumbnail cache is laid out in subdirectories that are made on demand.
    const QFileInfo produced(destinationPath());
    QVERIFY(produced.exists());
    QVERIFY(produced.size() > 0);

    // Read back rather than merely stat'ed. An unreadable file of non-zero length is
    // exactly what would be cached and then shown as a broken image forever.
    const QImage image(destinationPath());
    QVERIFY(!image.isNull());
}

void TestFrameExtractor::passesNostdinAndASingleFrame()
{
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    clearLog();

    QVERIFY(extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                   destinationPath(), 320));

    QCOMPARE(invocations().size(), 1);
    const QString arguments = invocations().first();

    // -nostdin is the flag that gets dropped in a refactor and then causes a hang
    // nobody can reproduce: ffmpeg reads standard input by default, so a child that
    // consumes the parent's can block waiting for input that never arrives.
    QVERIFY2(arguments.contains(QStringLiteral("-nostdin")), qPrintable(arguments));

    // Without this ffmpeg writes a numbered sequence of every frame it decodes, which
    // on a feature film fills the cache directory.
    QVERIFY2(arguments.contains(QStringLiteral("-frames:v 1")), qPrintable(arguments));
}

void TestFrameExtractor::passesAScaleFilterThatNeverUpscales()
{
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    clearLog();

    QVERIFY(extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                   destinationPath(), 320));

    const QString arguments = invocations().value(0);

    // min(iw,320) rather than a plain 320: a 160-pixel clip must stay 160 wide.
    // Enlarging it would cost bytes to add no detail whatsoever.
    QVERIFY2(arguments.contains(QStringLiteral("min(iw,320)")), qPrintable(arguments));
    QVERIFY2(arguments.contains(QStringLiteral("min(ih,320)")), qPrintable(arguments));
    QVERIFY2(arguments.contains(QStringLiteral("force_original_aspect_ratio=decrease")),
             qPrintable(arguments));

    // The filter has to be attached with -filter_complex and not -vf. A HEIF file is
    // stored as a grid of tiles, so ffmpeg is already running a complex filtergraph
    // to stitch it and rejects a simple -vf on the same stream — "Simple and complex
    // filtering cannot be used together for the same stream" — which fails the whole
    // invocation and produces no thumbnail at all.
    //
    // Asserted because no fixture can catch it: the stand-in binary ignores filters
    // entirely, so this was only found by running real ffmpeg against a real .heic.
    QVERIFY2(arguments.contains(QStringLiteral("-filter_complex")),
             qPrintable(arguments));
    QVERIFY2(!arguments.contains(QStringLiteral("-vf")), qPrintable(arguments));
}

void TestFrameExtractor::seeksPastTheStartForAPosterFrame()
{
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    clearLog();

    QVERIFY(extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                   destinationPath(), 320));

    const QString arguments = invocations().value(0);

    // Not frame zero. The first frame of a video is very often black, a fade-in or a
    // title card — and a folder of black rectangles satisfies "a thumbnail exists"
    // while completely failing the plan's criterion, which is that the frames be
    // distinguishable from one another.
    QVERIFY2(arguments.contains(QStringLiteral("-ss %1")
                                    .arg(FrameExtractor::PosterFrameSeconds)),
             qPrintable(arguments));

    // Before -i, so ffmpeg seeks using the container index instead of decoding and
    // throwing away every frame up to that point. On a long film that is the
    // difference between milliseconds and minutes.
    QVERIFY2(arguments.indexOf(QStringLiteral("-ss"))
                 < arguments.indexOf(QStringLiteral("-i")),
             qPrintable(arguments));
}

void TestFrameExtractor::doesNotSeekForAStillImage()
{
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    clearLog();

    QVERIFY(extractor.extractStill(sourcePath(QStringLiteral("photo.heic")),
                                   destinationPath(), 320));

    const QString arguments = invocations().value(0);

    // A photograph has one frame, and seeking three seconds into it finds nothing.
    QVERIFY2(!arguments.contains(QStringLiteral("-ss")), qPrintable(arguments));
}

void TestFrameExtractor::fallsBackToTheFirstFrameWhenTheClipIsShort()
{
    // The reason there is no ffprobe dependency. Learning a clip's duration would
    // mean a second binary and a second process spawned for every video, purely to
    // decide where to seek. A failed seek is already a perfectly good signal that the
    // clip is shorter than the offset, so the retry costs nothing in the common case
    // and nothing is needed to detect it.
    setFakeMode("seek_fails");

    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    clearLog();

    QString error;
    QVERIFY2(extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                    destinationPath(), 320, &error),
             qPrintable(error));

    const QStringList calls = invocations();
    QCOMPARE(calls.size(), 2);
    QVERIFY2(calls.at(0).contains(QStringLiteral("-ss")), qPrintable(calls.at(0)));
    QVERIFY2(!calls.at(1).contains(QStringLiteral("-ss")), qPrintable(calls.at(1)));

    QVERIFY(!QImage(destinationPath()).isNull());
}

// ─────────────────────────────────────────────────────────────────────────────
//  When ffmpeg misbehaves
// ─────────────────────────────────────────────────────────────────────────────

void TestFrameExtractor::treatsExitZeroWithNoOutputAsFailure()
{
    // ffmpeg really does this, and it is the single most important case here: a
    // success reported for a file that was never written would be recorded in the
    // thumbnail index, served with an immutable cache policy, and shown as a broken
    // image for as long as the file exists.
    setFakeMode("silent_success");

    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));

    QString error;
    QVERIFY2(!extractor.extractStill(sourcePath(QStringLiteral("photo.heic")),
                                     destinationPath(), 320, &error),
             "exit status 0 was trusted over whether a file appeared");
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(destinationPath()));
}

void TestFrameExtractor::treatsAnEmptyOutputFileAsFailure()
{
    setFakeMode("empty_file");

    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));

    QVERIFY(!extractor.extractStill(sourcePath(QStringLiteral("photo.heic")),
                                    destinationPath(), 320));

    // Removed, not merely reported. Left behind, it would sit in the cache directory
    // counting towards the size cap for ever.
    QVERIFY2(!QFileInfo::exists(destinationPath()),
             "the empty file was left behind");
}

void TestFrameExtractor::treatsUnreadableOutputAsFailure()
{
    // Non-zero length and still not an image. Only caught by actually trying to read
    // it, which is why ThumbnailCache re-decodes the still rather than trusting it.
    setFakeMode("garbage");

    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));

    // extractStill is content with a non-empty file — verifying that it decodes is
    // ThumbnailCache's job, and it does. What matters here is that something was
    // produced for it to reject.
    QVERIFY(extractor.extractStill(sourcePath(QStringLiteral("photo.heic")),
                                   destinationPath(), 320));
    QVERIFY(QImage(destinationPath()).isNull());
}

void TestFrameExtractor::killsAProcessThatNeverFinishes()
{
    // The failure that would be hardest to attribute in the field. The generator pool
    // is one or two threads, so a wedged ffmpeg does not merely lose its own
    // thumbnail — it stops every thumbnail on the machine, and the grid simply never
    // fills in.
    setFakeMode("hang");

    FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    extractor.setTimeoutMs(1500);
    QCOMPARE(extractor.timeoutMs(), 1500);

    QElapsedTimer timer;
    timer.start();

    QString error;
    QVERIFY(!extractor.extractStill(sourcePath(QStringLiteral("photo.heic")),
                                    destinationPath(), 320, &error));

    QVERIFY2(timer.elapsed() < 15'000,
             "the extractor waited far longer than its timeout");
    QVERIFY2(error.contains(QStringLiteral("did not finish")), qPrintable(error));
}

void TestFrameExtractor::rejectsANonPositiveLongEdge()
{
    // A zero would reach ffmpeg as scale=w='min(iw,0)', which is not an error it
    // reports usefully. Refusing here keeps the diagnosis local.
    const FrameExtractor extractor(QStringLiteral(FAKE_FFMPEG_PATH));
    clearLog();

    QVERIFY(!extractor.extractStill(sourcePath(QStringLiteral("clip.mp4")),
                                    destinationPath(), 0));
    QCOMPARE(invocations().size(), 0);
}

QTEST_GUILESS_MAIN(TestFrameExtractor)
#include "tst_frameextractor.moc"
