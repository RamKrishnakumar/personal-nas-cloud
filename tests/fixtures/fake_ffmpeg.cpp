// A stand-in for the ffmpeg CLI, used by tst_frameextractor.
//
// The real binary is ~80 MB, may not be installed, and its behaviour on a given file
// depends on how it was built — none of which makes for a test that says the same
// thing twice. What media::FrameExtractor actually has to get right is the plumbing:
// the arguments it builds, the output it checks for, and what it does when the
// process misbehaves. All of that is testable against a program that misbehaves on
// request, and only on request.
//
// Behaviour is chosen by the FAKE_FFMPEG_MODE environment variable:
//
//   ok              Write a valid PNG at the requested size. The default.
//   bad_version     Fail the -version probe, like a wrong-architecture binary.
//   seek_fails      Fail when -ss is present, succeed without it. A short clip.
//   silent_success  Exit 0 having written nothing. ffmpeg really does this.
//   empty_file      Exit 0 having written a zero-byte file.
//   garbage         Exit 0 having written bytes that are not an image.
//   hang            Never finish, so the timeout and kill can be exercised.
//
// Every invocation appends its arguments to FAKE_FFMPEG_LOG when that is set, which
// is how the tests assert on what was passed rather than only on what came back.

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <cstdio>

namespace {

QByteArray mode()
{
    const QByteArray value = qgetenv("FAKE_FFMPEG_MODE");
    return value.isEmpty() ? QByteArrayLiteral("ok") : value;
}

void logArguments(const QStringList& arguments)
{
    const QString logPath = QString::fromLocal8Bit(qgetenv("FAKE_FFMPEG_LOG"));
    if (logPath.isEmpty())
        return;

    QFile log(logPath);
    if (!log.open(QIODevice::Append | QIODevice::Text))
        return;

    QTextStream(&log) << arguments.join(QLatin1Char(' ')) << '\n';
}

} // namespace

/**
 * Stands in for ffprobe as well, selected by the name it was invoked under.
 *
 * VideoTranscoder looks for ffprobe beside ffmpeg by substituting the name, so the
 * fixture has to exist under both — the same binary built twice is simpler than two
 * sources that would drift apart.
 *
 * FAKE_PROBE_JSON overrides the reply, so a test can describe any source it likes
 * without needing a real video file of that shape.
 */
int runAsProbe(const QStringList& arguments)
{
    Q_UNUSED(arguments)

    const QByteArray override = qgetenv("FAKE_PROBE_JSON");
    if (!override.isEmpty()) {
        std::fwrite(override.constData(), 1, size_t(override.size()), stdout);
        return 0;
    }

    if (mode() == "probe_fails")
        return 1;

    // Default: the shape of the file this was all built for — 4K HEVC Main 10 HLG.
    static const char* const defaultJson = R"({
        "streams": [
            { "codec_type": "video", "codec_name": "hevc", "width": 3840,
              "height": 2160, "pix_fmt": "yuv420p10le",
              "color_transfer": "arib-std-b67" },
            { "codec_type": "audio", "codec_name": "aac" }
        ],
        "format": { "duration": "64.800000", "bit_rate": "83556079" }
    })";

    std::printf("%s\n", defaultJson);
    return 0;
}

/**
 * Stands in for a transcode: emits ffmpeg's progress stream, then writes a file.
 *
 * Progress is emitted in a handful of steps rather than continuously so a test can
 * assert that each one reached the callback, and slowly enough that a cancelling
 * callback has somewhere to cancel.
 *
 * Modes: `transcode_truncated` writes a file too small to be a real rendition,
 * `transcode_fails` exits non-zero after some progress, `transcode_slow` stretches
 * the run out so cancellation can be tested.
 */
int runAsTranscode(const QStringList& arguments)
{
    const QString output = arguments.last();
    const QByteArray selected = mode();

    const int steps = (selected == "transcode_slow") ? 100 : 5;
    const int pauseMs = (selected == "transcode_slow") ? 60 : 1;

    // 64.8 s, matching the probe reply, so percentages work out to round numbers.
    constexpr qint64 totalUs = 64'800'000;

    for (int step = 1; step <= steps; ++step) {
        const qint64 elapsedUs = (totalUs * step) / steps;

        std::printf("frame=%d\n", step * 10);
        std::printf("out_time_us=%lld\n", static_cast<long long>(elapsedUs));
        std::printf("progress=continue\n");
        std::fflush(stdout);

        QThread::msleep(pauseMs);

        if (selected == "transcode_fails" && step == 3)
            return 1;
    }

    std::printf("progress=end\n");
    std::fflush(stdout);

    QFile file(output);
    if (!file.open(QIODevice::WriteOnly))
        return 1;

    // Over the 1 KB floor for a success, under it for the truncated case.
    file.write(QByteArray(selected == "transcode_truncated" ? 64 : 8192, 'v'));
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments().mid(1);

    logArguments(arguments);

    if (QFileInfo(QCoreApplication::applicationFilePath())
            .fileName()
            .contains(QLatin1String("ffprobe"), Qt::CaseInsensitive)) {
        return runAsProbe(arguments);
    }

    const QByteArray selected = mode();

    // ── The version probe ─────────────────────────────────────────────────
    if (arguments.contains(QStringLiteral("-version"))) {
        if (selected == "bad_version")
            return 1;

        std::printf("ffmpeg version 0.0-fake Copyright (c) nobody\n");
        return 0;
    }

    if (selected == "hang") {
        // Longer than any timeout under test. The point is to be killed.
        QThread::sleep(600);
        return 0;
    }

    // ── Transcoding ───────────────────────────────────────────────────────
    // Recognised by the flag VideoTranscoder passes and nothing else does.
    if (arguments.contains(QStringLiteral("-progress")))
        return runAsTranscode(arguments);

    // The output path is the last argument, as it is for the real thing.
    if (arguments.isEmpty())
        return 1;
    const QString outputPath = arguments.last();

    if (selected == "silent_success")
        return 0;                      // Claims success, writes nothing.

    if (selected == "seek_fails" && arguments.contains(QStringLiteral("-ss")))
        return 1;                      // The clip is shorter than the seek offset.

    if (selected == "empty_file") {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly))
            return 1;
        return 0;                      // Zero bytes.
    }

    if (selected == "garbage") {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly))
            return 1;
        file.write("this is not a picture");
        return 0;
    }

    // ── ok ────────────────────────────────────────────────────────────────
    // A recognisable image rather than a blank one, so a test can tell a real
    // decode from an accidentally-empty buffer.
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);

    return image.save(outputPath, "PNG") ? 0 : 1;
}
