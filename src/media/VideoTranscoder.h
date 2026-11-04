#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace media {

/** What ffprobe says about a source video. */
struct VideoProfile
{
    qint64  durationMs = 0;
    int     width      = 0;
    int     height     = 0;
    qint64  bitrateBps = 0;
    QString videoCodec;
    QString audioCodec;
    QString pixelFormat;
    QString colourTransfer;

    bool isValid() const { return durationMs > 0 && width > 0 && height > 0; }

    /** 10-bit or wider. Browsers will not decode 10-bit H.264 or HEVC. */
    bool isWideGamutDepth() const { return pixelFormat.contains(QLatin1String("10"))
                                        || pixelFormat.contains(QLatin1String("12")); }

    /** HLG or PQ. Shown without tone mapping, these look washed out and grey. */
    bool isHdr() const
    {
        return colourTransfer == QLatin1String("arib-std-b67")      // HLG
            || colourTransfer == QLatin1String("smpte2084");        // PQ
    }
};

/** Whether a source is worth spending minutes of CPU on. */
enum class TranscodeNeed
{
    Unknown,      ///< Could not probe it; leave it alone.
    NotNeeded,    ///< Already something a browser streams comfortably.
    Recommended,  ///< Too big, too deep, or in a codec browsers refuse.
};

/**
 * @class VideoTranscoder
 * @brief Produces a browser-friendly rendition of a video, via ffmpeg.
 *
 * ### The problem this solves
 *
 * A clip from the owner's library: 3840x2160, HEVC Main 10, HLG HDR, **83.5 Mbps**,
 * 645 MB for 65 seconds. Netflix streams 4K at about 15 Mbps. Measured in §5.9, the
 * server delivers that file at well over 2 Gbps and the disk is nowhere near
 * saturated — the bottleneck is entirely the link to the phone, which realistically
 * carries 50-150 Mbps on home WiFi and 10-40 Mbps over a WAN upload. Nothing
 * server-side fixes a file whose bitrate exceeds the pipe.
 *
 * At CRF 23 the same content comes out at **1.25 Mbps** and about 10 MB a minute: a
 * 67x reduction, which fits any link. It is also small enough for a browser to
 * cache, which is what makes replay instant — a 645 MB response never survives in
 * any browser cache however correct the headers are.
 *
 * ### Why this is slow, and why that cannot be fixed here
 *
 * Measured on the reference i7-6500U (Skylake, 2 cores, 15 W), transcoding a 4K
 * 10-bit HEVC source runs at **0.24x realtime** — roughly four minutes of work per
 * minute of video. The breakdown, for ten seconds of source:
 *
 * | Stage | Cost |
 * |---|---|
 * | Decode only | 30 s |
 * | Decode + encode | 37 s |
 * | Decode + encode + tone mapping | 41 s |
 *
 * **Decoding is 73% of it.** Skylake's Quick Sync has no 10-bit HEVC decode — that
 * arrived with Kaby Lake — so it runs in software on two cores and there is no knob
 * that helps. Encoding is 17%, tone mapping 10%, which is why tone mapping is simply
 * always on rather than offered as a quality trade: skipping it would save a tenth of
 * the time and make every HDR video look grey.
 *
 * Two consequences follow and are worth stating plainly:
 *
 * - **Playback cannot begin before the transcode finishes.** At 0.24x realtime a
 *   player would outrun the encoder immediately, so segmenting the output to start
 *   early — the usual HLS trick — does not work on this hardware.
 * - **assess() matters more than the encoder does.** A 1080p H.264 phone video needs
 *   no transcode at all, and spending four minutes per minute discovering that would
 *   be the worst possible outcome.
 *
 * Const and stateless after construction; one instance is shared, and each call
 * creates its own process.
 */
class VideoTranscoder
{
public:
    /** Long edge of the rendition. 1080p is the sweet spot for phones and laptops. */
    static constexpr int TargetHeight = 1080;

    /**
     * @brief Above this, a source is considered too fat to stream as-is.
     *
     * 8 Mbps is comfortably within home WiFi and at the top of what a typical WAN
     * upload sustains. Below it, serving the original beats spending minutes of CPU
     * to save bytes nobody is short of.
     */
    static constexpr qint64 BitrateCeilingBps = 8'000'000;

    /** Codecs a browser can be relied on to decode. */
    static QStringList browserSafeVideoCodecs();

    /**
     * @param ffmpegPath Resolved ffmpeg binary. ffprobe is looked for beside it,
     *                   which is how every distribution ships the pair.
     */
    explicit VideoTranscoder(const QString& ffmpegPath);

    bool isAvailable() const { return m_available; }

    QString ffmpegPath() const { return m_ffmpeg; }
    QString ffprobePath() const { return m_ffprobe; }

    /** Reads duration, dimensions, bitrate and colour properties. */
    std::optional<VideoProfile> probe(const QString& path) const;

    /**
     * @brief Decides whether @p profile is worth transcoding.
     *
     * @param reason Filled with a short human-readable explanation, for the log and
     *               for the interface — "already 1080p H.264" is a far better thing
     *               to show someone than a silent refusal.
     */
    static TranscodeNeed assess(const VideoProfile& profile, QString* reason = nullptr);

    /**
     * @brief Called as the transcode advances. Return false to abort it.
     *
     * @param percent 0-100, derived from ffmpeg's own progress stream against the
     *                probed duration.
     */
    using ProgressCallback = std::function<bool(int percent)>;

    /**
     * @brief Writes a 1080p H.264 rendition of @p source to @p destination.
     *
     * Written to a neighbouring temporary file and renamed on success, so a crash or
     * a cancellation never leaves a half-encoded file looking like a finished one.
     *
     * @param profile Must come from probe(): the duration drives progress reporting,
     *                and the colour properties decide whether tone mapping is added.
     * @return true when @p destination exists and holds a complete rendition.
     */
    bool transcode(const QString&          source,
                   const QString&          destination,
                   const VideoProfile&     profile,
                   const ProgressCallback& progress = {},
                   QString*                error    = nullptr) const;

    /** The filter chain for @p profile, exposed so tests can assert on it. */
    static QString filterChain(const VideoProfile& profile);

private:
    QString m_ffmpeg;
    QString m_ffprobe;
    bool    m_available = false;
};

} // namespace media
