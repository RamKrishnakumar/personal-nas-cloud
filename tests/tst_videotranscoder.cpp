#include <QTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "media/VideoTranscoder.h"

using media::TranscodeNeed;
using media::VideoProfile;
using media::VideoTranscoder;

namespace {

void setFakeMode(const QByteArray& mode)
{
    qputenv("FAKE_FFMPEG_MODE", mode);
}

/** A profile that needs nothing doing to it, as a base for the assess() cases. */
VideoProfile acceptableProfile()
{
    VideoProfile profile;
    profile.durationMs     = 60'000;
    profile.width          = 1920;
    profile.height         = 1080;
    profile.bitrateBps     = 5'000'000;
    profile.videoCodec     = QStringLiteral("h264");
    profile.audioCodec     = QStringLiteral("aac");
    profile.pixelFormat    = QStringLiteral("yuv420p");
    profile.colourTransfer = QStringLiteral("bt709");
    return profile;
}

} // namespace

/**
 * Unit tests for media::VideoTranscoder, against the fake_ffmpeg fixture.
 *
 * The real binary is not used, for the reasons in tst_frameextractor — with one extra
 * reason here: a real transcode of the file this was built for takes four minutes per
 * minute of video, which is not something a test suite can contain.
 *
 * assess() gets the most attention, and deliberately. It decides whether to spend
 * minutes of CPU, and both of its mistakes are expensive: refusing a file that needed
 * converting leaves playback broken, and accepting one that did not burns four minutes
 * per minute of video to produce a file no better than the original.
 */
class TestVideoTranscoder : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ── Deciding whether to bother ───────────────────────────────────────
    void leavesAnAlreadyStreamableFileAlone();
    void recommendsForACodecBrowsersRefuse();
    void recommendsForMoreThanEightBits();
    void recommendsForHdr();
    void recommendsForMoreThan1080p();
    void recommendsForAnUnservableBitrate();
    void reportsUnknownForAnUnprobedFile();
    void everyVerdictExplainsItself();

    // ── The filter chain ─────────────────────────────────────────────────
    void toneMapsOnlyWhenTheSourceIsHdr();
    void neverUpscalesASmallSource();

    // ── Finding the tools ────────────────────────────────────────────────
    void needsFfprobeBesideFfmpeg();
    void probeReadsTheSourceProperties();
    void probeFailureIsReportedAsNoProfile();

    // ── Transcoding ──────────────────────────────────────────────────────
    void statesTheContainerExplicitly();
    void producesAFileAndReportsProgress();
    void progressNeverExceedsOneHundredBeforeTheEnd();
    void aCancellingCallbackStopsTheWork();
    void rejectsATruncatedRendition();
    void reportsAFailedEncode();
    void leavesNoPartialFileBehind();
    void refusesWithoutAProbedProfile();

private:
    QString source() const { return m_tmp.filePath(QStringLiteral("clip.mov")); }
    QString destination() const
    {
        return m_tmp.filePath(QStringLiteral("out/clip.mp4"));
    }

    QTemporaryDir m_tmp;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestVideoTranscoder::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    QVERIFY2(QFileInfo::exists(QStringLiteral(FAKE_FFMPEG_PATH)),
             "the fake_ffmpeg fixture was not built");
    QVERIFY2(QFileInfo::exists(QStringLiteral(FAKE_FFPROBE_PATH)),
             "the fake_ffprobe fixture was not built");

    QFile file(source());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'm'));
}

void TestVideoTranscoder::init()
{
    setFakeMode("ok");
    qunsetenv("FAKE_PROBE_JSON");
    QFile::remove(destination());
    QFile::remove(destination() + QStringLiteral(".part"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Deciding whether to bother
// ─────────────────────────────────────────────────────────────────────────────

void TestVideoTranscoder::leavesAnAlreadyStreamableFileAlone()
{
    // The case that saves the most work. A 1080p H.264 phone video plays fine as it
    // is, and converting it would cost minutes of CPU to produce something no better.
    QString reason;
    QCOMPARE(VideoTranscoder::assess(acceptableProfile(), &reason),
             TranscodeNeed::NotNeeded);
    QVERIFY2(reason.contains(QStringLiteral("already")), qPrintable(reason));
}

void TestVideoTranscoder::recommendsForACodecBrowsersRefuse()
{
    // HEVC is the iPhone default and is exactly the problem: Safari plays it, Chrome
    // on Android frequently cannot, and "sometimes" is no basis for serving a file.
    VideoProfile profile = acceptableProfile();
    profile.videoCodec = QStringLiteral("hevc");

    QString reason;
    QCOMPARE(VideoTranscoder::assess(profile, &reason), TranscodeNeed::Recommended);
    QVERIFY2(reason.contains(QStringLiteral("hevc")), qPrintable(reason));

    // The safe list is asserted rather than assumed, because quietly adding HEVC to it
    // would make videos stop playing on half the devices in a household.
    const QStringList safe = VideoTranscoder::browserSafeVideoCodecs();
    QVERIFY(safe.contains(QStringLiteral("h264")));
    QVERIFY2(!safe.contains(QStringLiteral("hevc")),
             "HEVC must not be treated as browser-safe");
}

void TestVideoTranscoder::recommendsForMoreThanEightBits()
{
    // No browser decodes 10-bit H.264, so a 10-bit source has to be converted even
    // when everything else about it is fine.
    VideoProfile profile = acceptableProfile();
    profile.pixelFormat = QStringLiteral("yuv420p10le");

    QVERIFY(profile.isWideGamutDepth());
    QCOMPARE(VideoTranscoder::assess(profile), TranscodeNeed::Recommended);
}

void TestVideoTranscoder::recommendsForHdr()
{
    // Served untouched, an HLG source looks washed out and grey — the one defect
    // people notice straight away.
    VideoProfile profile = acceptableProfile();
    profile.colourTransfer = QStringLiteral("arib-std-b67");
    QVERIFY(profile.isHdr());
    QCOMPARE(VideoTranscoder::assess(profile), TranscodeNeed::Recommended);

    profile.colourTransfer = QStringLiteral("smpte2084");       // PQ
    QVERIFY(profile.isHdr());
    QCOMPARE(VideoTranscoder::assess(profile), TranscodeNeed::Recommended);

    profile.colourTransfer = QStringLiteral("bt709");
    QVERIFY(!profile.isHdr());
}

void TestVideoTranscoder::recommendsForMoreThan1080p()
{
    VideoProfile profile = acceptableProfile();
    profile.width  = 3840;
    profile.height = 2160;

    QString reason;
    QCOMPARE(VideoTranscoder::assess(profile, &reason), TranscodeNeed::Recommended);
    QVERIFY2(reason.contains(QStringLiteral("2160")), qPrintable(reason));
}

void TestVideoTranscoder::recommendsForAnUnservableBitrate()
{
    // The measured case: 83.5 Mbps against a home link that carries 50-150 Mbps at
    // best on WiFi and 10-40 over a WAN upload.
    VideoProfile profile = acceptableProfile();
    profile.bitrateBps = 83'556'079;

    QString reason;
    QCOMPARE(VideoTranscoder::assess(profile, &reason), TranscodeNeed::Recommended);
    QVERIFY2(reason.contains(QStringLiteral("83.6")), qPrintable(reason));

    // Just under the ceiling is left alone, so the boundary is not quietly inclusive.
    profile.bitrateBps = VideoTranscoder::BitrateCeilingBps;
    QCOMPARE(VideoTranscoder::assess(profile), TranscodeNeed::NotNeeded);
}

void TestVideoTranscoder::reportsUnknownForAnUnprobedFile()
{
    // Distinct from NotNeeded on purpose. "I could not read this" must not be
    // recorded as "this is fine", or a damaged file would be served as-is for ever.
    QString reason;
    QCOMPARE(VideoTranscoder::assess(VideoProfile{}, &reason), TranscodeNeed::Unknown);
    QVERIFY(!reason.isEmpty());
}

void TestVideoTranscoder::everyVerdictExplainsItself()
{
    // The reason is shown in the interface, not just logged — "already 1080p H.264"
    // is a far better thing to put in front of someone than a silent refusal.
    const QList<VideoProfile> cases = [] {
        QList<VideoProfile> all;
        all.append(acceptableProfile());

        VideoProfile hevc = acceptableProfile();
        hevc.videoCodec = QStringLiteral("hevc");
        all.append(hevc);

        VideoProfile fourK = acceptableProfile();
        fourK.height = 2160;
        all.append(fourK);

        return all;
    }();

    for (const VideoProfile& profile : cases) {
        QString reason;
        VideoTranscoder::assess(profile, &reason);
        QVERIFY2(!reason.isEmpty(), "every verdict must come with a reason");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  The filter chain
// ─────────────────────────────────────────────────────────────────────────────

void TestVideoTranscoder::toneMapsOnlyWhenTheSourceIsHdr()
{
    VideoProfile sdr = acceptableProfile();
    const QString plain = VideoTranscoder::filterChain(sdr);

    QVERIFY2(!plain.contains(QStringLiteral("tonemap")), qPrintable(plain));
    QVERIFY2(plain.contains(QStringLiteral("format=yuv420p")), qPrintable(plain));

    VideoProfile hdr = sdr;
    hdr.colourTransfer = QStringLiteral("arib-std-b67");
    const QString mapped = VideoTranscoder::filterChain(hdr);

    // Through linear light, not a direct curve swap: tone mapping in a non-linear
    // space clips the highlights instead of rolling them off.
    QVERIFY2(mapped.contains(QStringLiteral("zscale=t=linear")), qPrintable(mapped));
    QVERIFY2(mapped.contains(QStringLiteral("tonemap=tonemap=hable")),
             qPrintable(mapped));
    QVERIFY2(mapped.endsWith(QStringLiteral("format=yuv420p")), qPrintable(mapped));

    // Scaling comes first, so tone mapping — which is per-pixel — runs over a quarter
    // of the pixels on a 4K source.
    QVERIFY(mapped.indexOf(QStringLiteral("scale=w="))
            < mapped.indexOf(QStringLiteral("tonemap")));
}

void TestVideoTranscoder::neverUpscalesASmallSource()
{
    const QString chain = VideoTranscoder::filterChain(acceptableProfile());

    // min() rather than a flat 1920x1080: a 720p clip must stay 720p, because
    // enlarging it costs bytes and encode time to add no detail at all.
    QVERIFY2(chain.contains(QStringLiteral("min(iw,1920)")), qPrintable(chain));
    QVERIFY2(chain.contains(QStringLiteral("min(ih,1080)")), qPrintable(chain));
    QVERIFY2(chain.contains(QStringLiteral("force_original_aspect_ratio=decrease")),
             qPrintable(chain));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Finding the tools
// ─────────────────────────────────────────────────────────────────────────────

void TestVideoTranscoder::needsFfprobeBesideFfmpeg()
{
    const VideoTranscoder found(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(found.isAvailable());
    QCOMPARE(QFileInfo(found.ffprobePath()).canonicalFilePath(),
             QFileInfo(QStringLiteral(FAKE_FFPROBE_PATH)).canonicalFilePath());

    // Without ffprobe there is no duration, so no honest progress bar and no way to
    // tell a file that needs converting from one that does not. Refusing beats
    // converting everything blindly for minutes at a time.
    const QString lonely = m_tmp.filePath(QStringLiteral("ffmpeg-alone.exe"));
    QFile copy(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(copy.copy(lonely));

    const VideoTranscoder orphaned(lonely);
    QVERIFY2(!orphaned.isAvailable(),
             "ffmpeg without ffprobe beside it must not report itself available");

    // Braces, not parentheses: `VideoTranscoder nothing(QString())` is read as a
    // function declaration, which compiles and tests nothing.
    const VideoTranscoder nothing{ QString() };
    QVERIFY(!nothing.isAvailable());
}

void TestVideoTranscoder::probeReadsTheSourceProperties()
{
    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));

    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    // The fixture describes the file this was all built for.
    QCOMPARE(profile->width, 3840);
    QCOMPARE(profile->height, 2160);
    QCOMPARE(profile->videoCodec, QStringLiteral("hevc"));
    QCOMPARE(profile->audioCodec, QStringLiteral("aac"));
    QCOMPARE(profile->pixelFormat, QStringLiteral("yuv420p10le"));
    QCOMPARE(profile->durationMs, 64'800);
    QCOMPARE(profile->bitrateBps, 83'556'079);
    QVERIFY(profile->isHdr());
    QVERIFY(profile->isWideGamutDepth());

    QCOMPARE(VideoTranscoder::assess(*profile), TranscodeNeed::Recommended);
}

void TestVideoTranscoder::probeFailureIsReportedAsNoProfile()
{
    setFakeMode("probe_fails");

    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    QVERIFY(!transcoder.probe(source()).has_value());

    // A reply with no video stream is just as unusable as no reply — an audio file
    // with a .mov name would land here.
    qputenv("FAKE_PROBE_JSON",
            R"({"streams":[{"codec_type":"audio","codec_name":"aac"}],)"
            R"("format":{"duration":"30.0","bit_rate":"128000"}})");
    setFakeMode("ok");
    QVERIFY(!transcoder.probe(source()).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Transcoding
// ─────────────────────────────────────────────────────────────────────────────

void TestVideoTranscoder::statesTheContainerExplicitly()
{
    // Found by running the real binary, not by any fixture. The output goes to
    // `<name>.mp4.part` so the rename into place is atomic — but ffmpeg infers the
    // container from the extension, and `.part` means nothing to it:
    //
    //   Unable to choose an output format for '...out.mp4.part'
    //
    // which fails the whole encode in under 100 ms. -f mp4 says it outright.
    const QString log = m_tmp.filePath(QStringLiteral("args.log"));
    QFile::remove(log);
    qputenv("FAKE_FFMPEG_LOG", log.toLocal8Bit());

    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());
    QVERIFY(transcoder.transcode(source(), destination(), *profile));

    QFile recorded(log);
    QVERIFY(recorded.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString arguments = QString::fromUtf8(recorded.readAll());

    QVERIFY2(arguments.contains(QStringLiteral("-f mp4")), qPrintable(arguments));

    // And the working file really is a .part, so the two have to stay consistent.
    QVERIFY2(arguments.contains(QStringLiteral(".part")), qPrintable(arguments));

    qunsetenv("FAKE_FFMPEG_LOG");
}

void TestVideoTranscoder::producesAFileAndReportsProgress()
{
    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    QList<int> seen;
    QString error;
    QVERIFY2(transcoder.transcode(source(), destination(), *profile,
                                  [&seen](int percent) {
                                      seen.append(percent);
                                      return true;
                                  }, &error),
             qPrintable(error));

    QVERIFY(QFileInfo::exists(destination()));

    // Progress has to actually arrive. A bar that sits at zero for four minutes and
    // then jumps to done is indistinguishable from a hang, which is the whole reason
    // ffmpeg's progress stream is parsed rather than ignored.
    QVERIFY2(seen.size() >= 2, "no intermediate progress was reported");
    QCOMPARE(seen.last(), 100);

    // Monotonic: a bar that goes backwards reads as a bug even when it is not.
    for (int i = 1; i < seen.size(); ++i)
        QVERIFY2(seen.at(i) >= seen.at(i - 1), "progress went backwards");
}

void TestVideoTranscoder::progressNeverExceedsOneHundredBeforeTheEnd()
{
    // The out_time_ms trap. ffmpeg has historically emitted that field in
    // microseconds, so reading it as milliseconds overstates progress a thousandfold
    // and the bar hits 100% almost immediately. out_time_us is parsed instead, and
    // this is what would catch a change back.
    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    QList<int> seen;
    QVERIFY(transcoder.transcode(source(), destination(), *profile,
                                 [&seen](int percent) {
                                     seen.append(percent);
                                     return true;
                                 }));

    for (int i = 0; i < seen.size() - 1; ++i) {
        QVERIFY2(seen.at(i) < 100,
                 qPrintable(QStringLiteral("reported %1%% before finishing")
                                .arg(seen.at(i))));
    }
}

void TestVideoTranscoder::aCancellingCallbackStopsTheWork()
{
    // Four minutes per minute of video means cancellation is not a nicety. Someone who
    // closes the page must not leave a laptop encoding for an hour.
    setFakeMode("transcode_slow");

    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    QElapsedTimer timer;
    timer.start();

    QString error;
    const bool finished = transcoder.transcode(
        source(), destination(), *profile,
        [](int percent) { return percent < 10; },     // Give up early.
        &error);

    QVERIFY2(!finished, "a cancelled transcode must not report success");
    QVERIFY2(error.contains(QStringLiteral("Cancelled")), qPrintable(error));

    // The fixture would run for about six seconds unmolested.
    QVERIFY2(timer.elapsed() < 5000,
             "the process was not killed promptly after cancellation");

    QVERIFY2(!QFileInfo::exists(destination()),
             "a cancelled transcode left an output file behind");
    QVERIFY2(!QFileInfo::exists(destination() + QStringLiteral(".part")),
             "a cancelled transcode left its working file behind");
}

void TestVideoTranscoder::rejectsATruncatedRendition()
{
    // ffmpeg can stop early having written a header and little else. Indexed and
    // served with a long cache lifetime, that plays as a clip that ends immediately
    // and there is nothing on screen to say why.
    setFakeMode("transcode_truncated");

    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    QString error;
    QVERIFY2(!transcoder.transcode(source(), destination(), *profile, {}, &error),
             "a file too small to be a rendition was accepted");
    QVERIFY(!QFileInfo::exists(destination()));
}

void TestVideoTranscoder::reportsAFailedEncode()
{
    setFakeMode("transcode_fails");

    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    QString error;
    QVERIFY(!transcoder.transcode(source(), destination(), *profile, {}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(destination()));
}

void TestVideoTranscoder::leavesNoPartialFileBehind()
{
    // Written to a neighbouring .part and renamed, so no failure mode leaves a
    // half-encoded file sitting in the cache looking finished.
    setFakeMode("transcode_fails");

    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));
    const auto profile = transcoder.probe(source());
    QVERIFY(profile.has_value());

    QVERIFY(!transcoder.transcode(source(), destination(), *profile));

    QDir outputDirectory(QFileInfo(destination()).absolutePath());
    const QStringList leftovers =
        outputDirectory.entryList({ QStringLiteral("*.part") }, QDir::Files);

    QVERIFY2(leftovers.isEmpty(),
             qPrintable(QStringLiteral("left behind: %1").arg(leftovers.join(u','))));
}

void TestVideoTranscoder::refusesWithoutAProbedProfile()
{
    // The duration drives progress reporting, so an unprobed profile would mean a bar
    // that never moves. Refusing says so immediately instead.
    const VideoTranscoder transcoder(QStringLiteral(FAKE_FFMPEG_PATH));

    QString error;
    QVERIFY(!transcoder.transcode(source(), destination(), VideoProfile{}, {}, &error));
    QVERIFY2(error.contains(QStringLiteral("probed")), qPrintable(error));
}

QTEST_GUILESS_MAIN(TestVideoTranscoder)
#include "tst_videotranscoder.moc"
