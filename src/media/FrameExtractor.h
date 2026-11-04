#pragma once

#include <QString>
#include <QStringList>

namespace media {

/**
 * @class FrameExtractor
 * @brief Pulls one still image out of a file Qt's image plugins cannot read.
 *
 * Videos and HEIC photos are the same problem — §5.7 of the v0.6 plan — and one
 * ffmpeg process solves both. Qt Image Formats has no HEIF plugin, so an iPhone
 * library has no thumbnails at all without this, and a folder of videos shows the
 * same film-strip icon for every entry.
 *
 * ### Why a subprocess
 *
 * Linking FFmpeg was rejected in the plan: Qt ships the runtime DLLs but no
 * matching development packages, so they would have to be sourced per compiler and
 * platform for a per-thumbnail saving nobody can perceive. A process per thumbnail
 * costs a few milliseconds of spawn time against tens of milliseconds of decode,
 * and the result is cached forever. QMediaPlayer frame grabbing was rejected too:
 * it is built for playback, it is asynchronous, and it does not help with HEIC.
 *
 * ### Finding the binary
 *
 * Three places, in order: the configured path, next to this executable, then `PATH`.
 * Resolved once on construction rather than per call — probing the filesystem for
 * every thumbnail is waste, and a path that changed halfway through a scan would be
 * a genuinely confusing bug to chase.
 *
 * When nothing is found the extractor reports unavailable and everything behaves as
 * it did before: videos keep their icon, HEIC photos keep the neutral placeholder.
 * That is the designed state, not a broken one — the binary is large and may not be
 * installed, so working without it has to be ordinary.
 *
 * ### Output
 *
 * A PNG at or below the requested long edge, written to a path the caller chooses.
 * Deliberately not the final thumbnail: ThumbnailCache then runs its existing Qt
 * path over that still, which keeps EXIF orientation, the pixel ceiling, the WebP
 * encoder and the quality setting all in one place instead of teaching ffmpeg about
 * cache tiers. The cost is decoding one small PNG.
 *
 * Const and stateless after construction, so one instance is shared by every
 * generator thread. QProcess is created per call, inside the calling thread.
 */
class FrameExtractor
{
public:
    /**
     * @brief Longest a single extraction may run before the process is killed.
     *
     * A malformed file can make ffmpeg spin, and the generator pool is one or two
     * threads — so without this one bad file stops every thumbnail on the machine,
     * not just its own.
     */
    static constexpr int DefaultTimeoutMs = 20'000;

    /**
     * @brief Where a poster frame is taken from, in seconds.
     *
     * Not zero. The first frame of a video is very often black, a fade-in or a
     * title card, and a folder of black rectangles satisfies "a thumbnail was
     * generated" while completely failing the thing thumbnails are for. A clip
     * shorter than this gets a second attempt at zero — see extractStill().
     */
    static constexpr int PosterFrameSeconds = 3;

    /**
     * @param executableOverride Configured path, tried first. May be empty.
     */
    explicit FrameExtractor(const QString& executableOverride = QString());

    /** True when a usable binary was found and answered a version query. */
    bool isAvailable() const { return m_available; }

    /** Absolute path of the binary in use, or empty when none was found. */
    QString executable() const { return m_executable; }

    /** First line of `ffmpeg -version`, for the startup log. */
    QString versionLine() const { return m_versionLine; }

    /**
     * @brief Overrides how long one extraction may run.
     *
     * The right number is hardware-dependent in the same way the stream slot count
     * is: seeking into a large file on a spinning disk is far slower than on an SSD,
     * and the reference i5 may legitimately need longer than the default while
     * someone on faster hardware would rather fail quickly. Values at or below zero
     * are ignored.
     */
    void setTimeoutMs(int milliseconds);
    int  timeoutMs() const { return m_timeoutMs; }

    /** True when @p path is a file this extractor should be asked about. */
    static bool handles(const QString& path);

    /** True for HEIF-family stills: .heic, .heif, .hif, .avif. */
    static bool isHeifStill(const QString& path);

    /** True for the video extensions worth attempting a poster frame for. */
    static bool isVideo(const QString& path);

    /**
     * @brief Every suffix handles() accepts, lowercase and sorted.
     *
     * Feeds ThumbnailCache::capabilityFingerprint(), which the background scanner
     * uses to notice that it can now read file types it previously skipped. Sorted
     * so the fingerprint does not change when this list is merely reordered.
     */
    static QStringList handledSuffixes();

    /**
     * @brief Writes a still from @p sourcePath to @p destinationPath as PNG.
     *
     * For a video this is a frame PosterFrameSeconds in, falling back to the first
     * frame when the clip is shorter than that. The fallback is what avoids needing
     * ffprobe and a second process just to learn the duration: a seek past the end
     * produces no output, which is a perfectly good signal to try again at zero.
     *
     * Neither dimension of the result exceeds @p longEdge, and a source smaller than
     * that is never upscaled — enlarging it would cost bytes to add no detail.
     *
     * @return true when @p destinationPath exists and is non-empty afterwards.
     */
    bool extractStill(const QString& sourcePath,
                      const QString& destinationPath,
                      int            longEdge,
                      QString*       error = nullptr) const;

private:
    /** Runs one ffmpeg invocation and reports whether it produced output. */
    bool runOnce(const QStringList& arguments,
                 const QString&     destinationPath,
                 QString*           error) const;

    /** Searches the configured path, the application directory, then PATH. */
    static QString resolveExecutable(const QString& executableOverride);

    QString m_executable;
    QString m_versionLine;
    bool    m_available = false;
    int     m_timeoutMs = DefaultTimeoutMs;
};

} // namespace media
