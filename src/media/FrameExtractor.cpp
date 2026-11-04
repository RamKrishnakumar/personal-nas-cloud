#include "media/FrameExtractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace {

/** Names to look for. The `.exe` is implicit on Windows via QFile::exists checks. */
const char* const ExecutableNames[] = {
#ifdef Q_OS_WIN
    "ffmpeg.exe",
#endif
    "ffmpeg",
};

const QSet<QString>& heifSuffixes()
{
    // AVIF is here with the HEIF family because it is the same container lineage and
    // Qt has no plugin for it either. Newer phones produce it.
    static const QSet<QString> suffixes = {
        QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("hif"),  QStringLiteral("avif"),
    };
    return suffixes;
}

const QSet<QString>& videoSuffixes()
{
    // Container extensions, not codecs. Being wrong in the permissive direction is
    // cheap — one failed extraction, remembered by the decode-failure table and never
    // retried — while being wrong in the other direction means a whole family of
    // files silently keeps the generic icon.
    static const QSet<QString> suffixes = {
        QStringLiteral("mp4"),  QStringLiteral("m4v"),  QStringLiteral("mov"),
        QStringLiteral("mkv"),  QStringLiteral("webm"), QStringLiteral("avi"),
        QStringLiteral("wmv"),  QStringLiteral("flv"),  QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("3gp"),  QStringLiteral("ts"),
        QStringLiteral("m2ts"), QStringLiteral("mts"),  QStringLiteral("ogv"),
    };
    return suffixes;
}

QString suffixOf(const QString& path)
{
    return QFileInfo(path).suffix().toLower();
}

} // namespace

namespace media {

FrameExtractor::FrameExtractor(const QString& executableOverride)
{
    m_executable = resolveExecutable(executableOverride);

    if (m_executable.isEmpty()) {
        // Information, not a warning. A machine without ffmpeg is a supported
        // configuration — it simply shows placeholders — and shouting about it would
        // train people to ignore the log.
        qInfo() << "[Frames] No ffmpeg binary found. Videos and HEIC photos will show "
                   "a placeholder. Put ffmpeg next to the application, or set "
                   "ffmpegPath in config.ini, to enable them.";
        return;
    }

    // Asked to prove itself rather than trusted because it exists. A file called
    // ffmpeg that is a broken symlink, a wrapper script with a missing interpreter or
    // a binary for the wrong architecture all pass an existence check and then fail
    // on every thumbnail instead of once, here.
    QProcess probe;
    probe.start(m_executable, { QStringLiteral("-version") });

    if (!probe.waitForFinished(10'000) || probe.exitCode() != 0) {
        qWarning() << "[Frames] Found" << m_executable
                   << "but it did not answer -version; ignoring it.";
        m_executable.clear();
        return;
    }

    m_versionLine = QString::fromUtf8(probe.readAllStandardOutput())
                        .split(QLatin1Char('\n')).value(0).trimmed();
    m_available = true;

    qInfo() << "[Frames] Using" << m_executable;
    if (!m_versionLine.isEmpty())
        qInfo() << "[Frames]" << m_versionLine;
}

void FrameExtractor::setTimeoutMs(int milliseconds)
{
    if (milliseconds > 0)
        m_timeoutMs = milliseconds;
}

QString FrameExtractor::resolveExecutable(const QString& executableOverride)
{
    // 1. Configured explicitly. Checked first so a deliberate choice always wins over
    //    whatever happens to be installed.
    if (!executableOverride.isEmpty()) {
        const QFileInfo configured(executableOverride);
        if (configured.isFile() && configured.isExecutable())
            return configured.absoluteFilePath();

        qWarning() << "[Frames] Configured ffmpeg path is not a runnable file:"
                   << executableOverride << "— falling back to the usual places.";
    }

    // 2. Beside the application. This is what a bundled installer produces, and it
    //    takes precedence over PATH so the shipped version is the one used rather
    //    than some older copy already on the machine.
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    if (!applicationDirectory.isEmpty()) {
        for (const char* name : ExecutableNames) {
            const QFileInfo candidate(applicationDirectory + QLatin1Char('/')
                                      + QLatin1String(name));
            if (candidate.isFile() && candidate.isExecutable())
                return candidate.absoluteFilePath();
        }
    }

    // 3. On PATH, which is how it will usually be found during development.
    for (const char* name : ExecutableNames) {
        const QString found = QStandardPaths::findExecutable(QLatin1String(name));
        if (!found.isEmpty())
            return found;
    }

    return {};
}

bool FrameExtractor::isHeifStill(const QString& path)
{
    return heifSuffixes().contains(suffixOf(path));
}

bool FrameExtractor::isVideo(const QString& path)
{
    return videoSuffixes().contains(suffixOf(path));
}

bool FrameExtractor::handles(const QString& path)
{
    return isHeifStill(path) || isVideo(path);
}

QStringList FrameExtractor::handledSuffixes()
{
    QStringList suffixes = QStringList(heifSuffixes().cbegin(), heifSuffixes().cend())
                           + QStringList(videoSuffixes().cbegin(),
                                         videoSuffixes().cend());

    // QSet iteration order is unspecified and varies between runs, so sorting is what
    // makes the fingerprint built from this stable.
    suffixes.sort();
    return suffixes;
}

bool FrameExtractor::extractStill(const QString& sourcePath,
                                  const QString& destinationPath,
                                  int            longEdge,
                                  QString*       error) const
{
    if (!m_available) {
        if (error)
            *error = QStringLiteral("No ffmpeg binary is available.");
        return false;
    }

    if (longEdge <= 0) {
        if (error)
            *error = QStringLiteral("A positive long edge is required.");
        return false;
    }

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());

    // Caps both dimensions and never enlarges: min(iw,N) leaves a small source at its
    // own size, and force_original_aspect_ratio=decrease keeps the shape. Without the
    // min() a 320-pixel clip would be blown up to 1280 — more bytes, no more detail.
    const QString scaleFilter =
        QStringLiteral("scale=w='min(iw,%1)':h='min(ih,%1)':"
                       "force_original_aspect_ratio=decrease")
            .arg(longEdge);

    // -nostdin is not optional. ffmpeg reads standard input by default, and a child
    // that consumes the parent's stdin interferes with the console it was launched
    // from and can block waiting for input that never comes.
    QStringList arguments = {
        QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"),                       // Overwrite our own temp file.
    };

    const bool video = isVideo(sourcePath);

    if (video) {
        // Before -i, which is input seeking: ffmpeg jumps using the container index
        // instead of decoding and discarding every frame up to that point. For a long
        // film that is the difference between milliseconds and minutes.
        arguments << QStringLiteral("-ss")
                  << QString::number(PosterFrameSeconds);
    }

    arguments << QStringLiteral("-i") << QDir::toNativeSeparators(sourcePath)
              << QStringLiteral("-frames:v") << QStringLiteral("1")

              // -filter_complex, not -vf, and this is not interchangeable. A HEIF
              // file stores its image as a grid of 512x512 tiles, so ffmpeg builds a
              // complex filtergraph internally to stitch them — and it then refuses
              // a simple -vf on the same stream:
              //
              //   Simple and complex filtering cannot be used together for the
              //   same stream
              //
              // which fails the whole invocation. -filter_complex produces identical
              // output for video, so one spelling serves both rather than branching.
              << QStringLiteral("-filter_complex") << scaleFilter

              << QStringLiteral("-f") << QStringLiteral("image2")
              << QDir::toNativeSeparators(destinationPath);

    if (runOnce(arguments, destinationPath, error))
        return true;

    if (!video)
        return false;

    // A clip shorter than the seek offset produces nothing, which is the whole reason
    // this retry exists: it gets a frame from a two-second video without ffprobe and
    // a second process spawned purely to ask how long the file is.
    QStringList fromTheStart = arguments;
    const qsizetype seekFlag = fromTheStart.indexOf(QStringLiteral("-ss"));
    if (seekFlag >= 0)
        fromTheStart.remove(seekFlag, 2);

    return runOnce(fromTheStart, destinationPath, error);
}

bool FrameExtractor::runOnce(const QStringList& arguments,
                             const QString&     destinationPath,
                             QString*           error) const
{
    QProcess process;

    // Merged so one read drains both and a chatty build cannot fill a pipe nobody is
    // reading, which would deadlock the process we are waiting on.
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(m_executable, arguments);

    if (!process.waitForStarted(5'000)) {
        if (error)
            *error = QStringLiteral("ffmpeg did not start: %1").arg(process.errorString());
        return false;
    }

    if (!process.waitForFinished(m_timeoutMs)) {
        // kill() rather than terminate(): this has already had its full budget, and a
        // polite request can be ignored by exactly the wedged process that needs
        // stopping. The generator pool is one or two threads, so a process left
        // running here stops thumbnails for the whole machine.
        process.kill();
        process.waitForFinished(2'000);

        QFile::remove(destinationPath);

        if (error) {
            *error = QStringLiteral("ffmpeg did not finish within %1 ms")
                         .arg(m_timeoutMs);
        }
        return false;
    }

    const QString output = QString::fromUtf8(process.readAll()).trimmed();

    // Exit status alone is not enough. ffmpeg can report success having written
    // nothing — a seek past the end of a short clip is the usual way — and an empty
    // file would be cached as a valid thumbnail and shown as a broken image forever.
    const QFileInfo produced(destinationPath);
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
        && produced.exists() && produced.size() > 0) {
        return true;
    }

    QFile::remove(destinationPath);

    if (error) {
        *error = output.isEmpty()
                     ? QStringLiteral("ffmpeg exited %1 without producing an image")
                           .arg(process.exitCode())
                     : output;
    }
    return false;
}

} // namespace media
