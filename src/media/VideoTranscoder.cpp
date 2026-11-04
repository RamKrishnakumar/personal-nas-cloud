#include "media/VideoTranscoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QThread>

namespace {

/** ffprobe lives beside ffmpeg in every distribution that ships either. */
QString probeBeside(const QString& ffmpegPath)
{
    const QFileInfo info(ffmpegPath);
    if (!info.isFile())
        return {};

    QString name = info.fileName();
    name.replace(QLatin1String("ffmpeg"), QLatin1String("ffprobe"), Qt::CaseInsensitive);
    if (name == info.fileName())
        return {};                     // Not a name we recognise; do not guess.

    const QFileInfo candidate(info.absolutePath() + QLatin1Char('/') + name);
    return candidate.isFile() && candidate.isExecutable()
               ? candidate.absoluteFilePath()
               : QString();
}

/**
 * The HLG/PQ to SDR chain.
 *
 * Shown without this, an HDR source looks washed out and grey — the one visual
 * defect people notice immediately. The route through linear light and gbrpf32le is
 * the standard one: tone mapping has to happen in a linear float space or the
 * highlights clip instead of rolling off. `hable` is chosen over `reinhard` because it
 * preserves contrast in the midtones, which is where faces are.
 */
constexpr auto ToneMapChain =
    "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
    "tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p";

} // namespace

namespace media {

QStringList VideoTranscoder::browserSafeVideoCodecs()
{
    // H.264 is the only one every browser and phone decodes. VP8/VP9/AV1 are safe on
    // current desktop browsers. HEVC is deliberately absent: Safari plays it, Chrome
    // on Android often cannot, and "sometimes" is not a basis for serving a file.
    return { QStringLiteral("h264"), QStringLiteral("vp8"),
             QStringLiteral("vp9"),  QStringLiteral("av1") };
}

VideoTranscoder::VideoTranscoder(const QString& ffmpegPath)
    : m_ffmpeg(ffmpegPath)
{
    if (m_ffmpeg.isEmpty())
        return;

    m_ffprobe = probeBeside(m_ffmpeg);

    if (m_ffprobe.isEmpty()) {
        // Without ffprobe there is no duration, so no honest progress bar and no way
        // to tell a file that needs transcoding from one that does not. Refusing is
        // better than transcoding everything blindly for minutes at a time.
        qInfo() << "[Video] Found ffmpeg but no ffprobe beside it; video optimisation "
                   "is off.";
        return;
    }

    m_available = true;
}

std::optional<VideoProfile> VideoTranscoder::probe(const QString& path) const
{
    if (!m_available)
        return std::nullopt;

    QProcess process;
    process.start(m_ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-print_format"), QStringLiteral("json"),
        QStringLiteral("-show_entries"),
        QStringLiteral("format=duration,bit_rate:"
                       "stream=codec_type,codec_name,width,height,pix_fmt,"
                       "color_transfer"),
        QDir::toNativeSeparators(path),
    });

    // Generous but bounded. ffprobe on a damaged file can sit reading for a long time
    // looking for a stream header that is not there.
    if (!process.waitForFinished(30'000)) {
        process.kill();
        process.waitForFinished(2'000);
        return std::nullopt;
    }

    if (process.exitCode() != 0)
        return std::nullopt;

    const QJsonObject root =
        QJsonDocument::fromJson(process.readAllStandardOutput()).object();

    VideoProfile profile;

    const QJsonObject format = root.value(QStringLiteral("format")).toObject();
    profile.durationMs =
        qint64(format.value(QStringLiteral("duration")).toString().toDouble() * 1000.0);
    profile.bitrateBps = format.value(QStringLiteral("bit_rate")).toString().toLongLong();

    for (const QJsonValue& value : root.value(QStringLiteral("streams")).toArray()) {
        const QJsonObject stream = value.toObject();
        const QString type = stream.value(QStringLiteral("codec_type")).toString();

        if (type == QLatin1String("video") && profile.width == 0) {
            // First video stream only. An iPhone MOV also carries several `mebx`
            // metadata tracks, and one of them would otherwise overwrite this.
            profile.width  = stream.value(QStringLiteral("width")).toInt();
            profile.height = stream.value(QStringLiteral("height")).toInt();
            profile.videoCodec  = stream.value(QStringLiteral("codec_name")).toString();
            profile.pixelFormat = stream.value(QStringLiteral("pix_fmt")).toString();
            profile.colourTransfer =
                stream.value(QStringLiteral("color_transfer")).toString();
        } else if (type == QLatin1String("audio") && profile.audioCodec.isEmpty()) {
            profile.audioCodec = stream.value(QStringLiteral("codec_name")).toString();
        }
    }

    if (!profile.isValid())
        return std::nullopt;

    return profile;
}

TranscodeNeed VideoTranscoder::assess(const VideoProfile& profile, QString* reason)
{
    const auto conclude = [reason](TranscodeNeed need, const QString& because) {
        if (reason)
            *reason = because;
        return need;
    };

    if (!profile.isValid())
        return conclude(TranscodeNeed::Unknown,
                        QStringLiteral("could not be probed"));

    // Order matters only for the message: the first thing that disqualifies a file is
    // the one worth telling someone about.

    if (!browserSafeVideoCodecs().contains(profile.videoCodec)) {
        return conclude(TranscodeNeed::Recommended,
                        QStringLiteral("%1 is not reliably playable in browsers")
                            .arg(profile.videoCodec));
    }

    if (profile.isWideGamutDepth()) {
        return conclude(TranscodeNeed::Recommended,
                        QStringLiteral("%1 is deeper than 8-bit")
                            .arg(profile.pixelFormat));
    }

    if (profile.isHdr()) {
        return conclude(TranscodeNeed::Recommended,
                        QStringLiteral("HDR, which needs tone mapping to look right"));
    }

    if (profile.height > TargetHeight) {
        return conclude(TranscodeNeed::Recommended,
                        QStringLiteral("%1p is taller than %2p")
                            .arg(profile.height).arg(TargetHeight));
    }

    if (profile.bitrateBps > BitrateCeilingBps) {
        return conclude(TranscodeNeed::Recommended,
                        QStringLiteral("%1 Mbps is more than a home link carries")
                            .arg(profile.bitrateBps / 1'000'000.0, 0, 'f', 1));
    }

    // Everything a browser needs, already. Spending four minutes per minute of video
    // to discover that would be the worst outcome available.
    return conclude(TranscodeNeed::NotNeeded,
                    QStringLiteral("already %1p %2 at %3 Mbps")
                        .arg(profile.height)
                        .arg(profile.videoCodec)
                        .arg(profile.bitrateBps / 1'000'000.0, 0, 'f', 1));
}

QString VideoTranscoder::filterChain(const VideoProfile& profile)
{
    // min() rather than a flat target: a 720p source stays 720p. Enlarging it costs
    // bytes and encode time to add no detail.
    QString chain = QStringLiteral("scale=w='min(iw,%1)':h='min(ih,%2)':"
                                   "force_original_aspect_ratio=decrease")
                        .arg(TargetHeight * 16 / 9)
                        .arg(TargetHeight);

    if (profile.isHdr()) {
        // Scaling first on purpose: tone mapping is per-pixel, so doing it after the
        // downscale is a quarter of the work on a 4K source.
        chain += QLatin1Char(',') + QLatin1String(ToneMapChain);
    } else {
        chain += QLatin1String(",format=yuv420p");
    }

    return chain;
}

bool VideoTranscoder::transcode(const QString&          source,
                                const QString&          destination,
                                const VideoProfile&     profile,
                                const ProgressCallback& progress,
                                QString*                error) const
{
    const auto fail = [error](const QString& because) {
        if (error)
            *error = because;
        return false;
    };

    if (!m_available)
        return fail(QStringLiteral("No ffmpeg is available."));

    if (!profile.isValid())
        return fail(QStringLiteral("The source was never probed."));

    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return fail(QStringLiteral("Could not create the output directory."));

    // Renamed into place at the end. A half-encoded file that looked finished would
    // be indexed, served with a long cache lifetime, and play as a truncated clip.
    const QString workingPath = destination + QStringLiteral(".part");
    QFile::remove(workingPath);

    const QStringList arguments = {
        QStringLiteral("-nostdin"),
        QStringLiteral("-nostats"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),

        // Machine-readable progress on stdout, leaving stderr for real errors.
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),

        QStringLiteral("-y"),
        QStringLiteral("-i"), QDir::toNativeSeparators(source),

        QStringLiteral("-vf"), filterChain(profile),

        QStringLiteral("-c:v"), QStringLiteral("libx264"),

        // veryfast, not slower. Encoding is 17% of the cost against decode's 73%, so a
        // slower preset buys a few percent of file size for a large share of the time
        // on a machine where time is the scarce thing.
        QStringLiteral("-preset"), QStringLiteral("veryfast"),

        // Quality-targeted rather than bitrate-targeted, so a static shot costs
        // little and a busy one gets what it needs.
        QStringLiteral("-crf"), QStringLiteral("23"),

        // The ceiling is the point of the exercise: CRF alone can spike well past the
        // link budget during high motion, which is exactly when stalling is worst.
        QStringLiteral("-maxrate"), QStringLiteral("6M"),
        QStringLiteral("-bufsize"), QStringLiteral("12M"),

        // 8-bit 4:2:0 High profile: the one combination every browser decodes.
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-profile:v"), QStringLiteral("high"),

        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-b:a"), QStringLiteral("128k"),
        QStringLiteral("-ac"), QStringLiteral("2"),

        // Drops the iPhone's `mebx` metadata tracks, which no browser reads and some
        // reject the container over.
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?"),

        // The index at the front, so a player can start without first fetching the
        // end of the file. Free here, because the file is being rewritten anyway.
        QStringLiteral("-movflags"), QStringLiteral("+faststart"),

        // Stated explicitly because the output is written to `<name>.mp4.part`, and
        // ffmpeg infers the container from the extension — `.part` means nothing to
        // it, so it refuses with "Unable to choose an output format". Writing to the
        // final name instead would be worse: it is the atomic rename that stops a
        // half-encoded file ever being indexed as a finished one.
        QStringLiteral("-f"), QStringLiteral("mp4"),

        QDir::toNativeSeparators(workingPath),
    };

    QProcess process;
    process.start(m_ffmpeg, arguments);

    if (!process.waitForStarted(10'000)) {
        QFile::remove(workingPath);
        return fail(QStringLiteral("ffmpeg did not start: %1").arg(process.errorString()));
    }

    bool       cancelled = false;
    int        lastPercent = -1;
    QByteArray pending;

    // No overall timeout. A long video legitimately takes a long time — four minutes
    // of work per minute of 4K on the reference machine — so a wall-clock limit would
    // either be uselessly large or would kill valid work. Progress is the liveness
    // signal instead: the caller sees percentages advancing and can cancel.
    while (process.state() != QProcess::NotRunning) {
        if (!process.waitForReadyRead(5'000)) {
            if (process.state() == QProcess::NotRunning)
                break;
            continue;                  // Quiet spell; still encoding.
        }

        pending += process.readAllStandardOutput();

        qsizetype newline = 0;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(newline).trimmed();
            pending.remove(0, newline + 1);

            // out_time_us rather than out_time_ms: the latter has historically been
            // emitted in microseconds too, so reading it as milliseconds reports a
            // thousandfold overshoot.
            if (!line.startsWith("out_time_us="))
                continue;

            const QByteArray value = line.mid(line.indexOf('=') + 1);
            if (value == "N/A")
                continue;

            const qint64 elapsedMs = value.toLongLong() / 1000;
            // Capped at 99 until the process actually finishes, so the only thing
            // that reports completion is completion.
            const int percent =
                profile.durationMs > 0
                    ? int(qBound<qint64>(0, elapsedMs * 100 / profile.durationMs,
                                         qint64(99)))
                    : 0;

            if (percent != lastPercent) {
                lastPercent = percent;
                if (progress && !progress(percent)) {
                    cancelled = true;
                    break;
                }
            }
        }

        if (cancelled)
            break;
    }

    if (cancelled) {
        // kill(), not terminate(): the caller has decided this work is unwanted, and a
        // polite request can be ignored by a process busy encoding.
        process.kill();
        process.waitForFinished(5'000);
        QFile::remove(workingPath);
        return fail(QStringLiteral("Cancelled."));
    }

    process.waitForFinished(30'000);

    const QByteArray complaints = process.readAllStandardError().trimmed();

    // Exit status is not enough on its own — ffmpeg can stop early having written a
    // truncated file — so the output is required to exist and be substantial.
    const QFileInfo produced(workingPath);
    const bool succeeded = process.exitStatus() == QProcess::NormalExit
                           && process.exitCode() == 0
                           && produced.exists()
                           && produced.size() > 1024;

    if (!succeeded) {
        QFile::remove(workingPath);
        return fail(complaints.isEmpty()
                        ? QStringLiteral("ffmpeg exited %1 without a usable file")
                              .arg(process.exitCode())
                        : QString::fromUtf8(complaints));
    }

    QFile::remove(destination);
    if (!QFile::rename(workingPath, destination)) {
        QFile::remove(workingPath);
        return fail(QStringLiteral("Could not move the rendition into place."));
    }

    if (progress)
        progress(100);

    return true;
}

} // namespace media
