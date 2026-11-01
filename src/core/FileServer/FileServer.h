#pragma once

#include <QObject>
#include <QSslConfiguration>
#include <QString>

#include <memory>

namespace auth {
class LoginRateLimiter;
class PermissionStore;
class SessionStore;
class UserStore;
}
namespace db       { class AuditLog; class Database; }
namespace handlers { class AuthGate; class AuthHandler; class FileRouter; }
namespace http     { class HttpServer; }
namespace media {
class FrameExtractor;
class ThumbnailCache;
class ThumbnailScanner;
class VideoStore;
class VideoTranscoder;
}

class QTimer;

/**
 * @class FileServer
 * @brief Owns the HTTP stack and exposes it to the UI.
 *
 * Since v0.6 this is a facade. The work lives in:
 *   - http::HttpServer  — accepting, and the worker thread pool
 *   - http::Worker      — socket ownership per thread, TLS handshakes
 *   - http::Connection  — framing, keep-alive, byte ranges, streaming
 *   - handlers::FileRouter — what a path means
 *
 * Request handling no longer runs on the GUI thread, so a large download cannot
 * stall the window the way it did in v0.5.
 *
 * The public signals are unchanged, so MainWindow needed no rework.
 */
class FileServer : public QObject
{
    Q_OBJECT

public:
    explicit FileServer(QObject* parent = nullptr);
    ~FileServer() override;

    // ── TLS ───────────────────────────────────────────────────────────────

    /**
     * @brief Loads a PEM certificate and RSA private key.
     *
     * Relative paths resolve against the executable's directory. Must be called
     * before start() when HTTPS is wanted.
     *
     * @return true on success; otherwise false with the reason in tlsErrorString().
     */
    bool loadTlsFiles(const QString& certificatePath, const QString& privateKeyPath);

    /**
     * @brief Swaps in a renewed certificate without interrupting anything.
     *
     * Safe to call while the server is listening, which is the whole point:
     * core::CertRenewer connects its certificateRenewed signal here, so a 90-day
     * certificate is replaced in place rather than needing the server stopped and
     * started. Connections already established keep the certificate they handshook
     * with; everything opened afterwards gets the new one.
     *
     * Must be called from the thread that owns this object — the same thread that
     * accepts connections. See http::HttpServer::setTlsConfiguration().
     */
    void reloadTls(const QSslConfiguration& configuration);

    void setUseHttps(bool enabled) { m_useHttps = enabled; }
    bool useHttps() const { return m_useHttps; }

    QString tlsErrorString() const { return m_tlsErrorString; }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool    start(quint16 port = 9090);
    void    stop();
    bool    isRunning() const;
    quint16 port() const { return m_port; }

    /**
     * @brief The address other devices on the same network should use.
     *
     * Deliberately an RFC1918 address and nothing else. Picking the first
     * non-loopback entry, as v0.5 did, lands on whichever virtual adapter happens to
     * be enumerated first and hands out an address nothing can reach.
     */
    QString localIP() const;

    /**
     * @brief The address inside the private overlay network, or empty.
     *
     * Separate from localIP() rather than folded into it, because both are true at
     * once and a machine has both: the LAN address works for devices in the house,
     * and this one works from anywhere. Showing only the first was the practical
     * effect of the overlay range not appearing in localIP()'s list at all.
     *
     * Recognised by address range — 100.64.0.0/10, which RFC 6598 reserves for
     * carrier-grade NAT and which Tailscale deliberately reuses. On a *local*
     * interface that is effectively conclusive: an ISP's own CGNAT addresses live on
     * their equipment, not on this laptop's adapters.
     *
     * Prefer the MagicDNS name over this when one is available. The certificate is
     * issued for the name, so connecting by address produces exactly the warning
     * that having a real certificate is meant to remove.
     */
    QString overlayIP() const;

    /** Live connection count across all worker threads. */
    int connectionCount() const;

    /** Concurrent metered transfers. */
    int activeStreams() const;

    /**
     * @brief Overrides the detected simultaneous-transfer limit.
     *
     * Zero or less restores automatic detection. Exposed because every detection
     * heuristic is wrong for somebody, and the right number depends on the
     * owner's upload bandwidth as much as on their hardware.
     */
    // The parameter is deliberately not called "slots": that is a Qt keyword
    // macro, and moc reads it as the start of a slots: section, failing with
    // "Missing access specifier for slots" on a line that looks fine.
    void setStreamSlotOverride(int count) { m_streamSlotOverride = count; }
    int  streamSlotOverride() const { return m_streamSlotOverride; }

    /** Root directory exposed to clients. Set before start(). */
    void setNasRoot(const QString& path);

    /**
     * @brief Enables thumbnail serving.
     *
     * Optional: without it the grid links originals, which works but is slow.
     * Must be called before start().
     */
    void setThumbnailStore(db::Database* database, const QString& cacheDirectory);

    /**
     * @brief Enables thumbnails for videos and HEIC photos via ffmpeg.
     *
     * Must be called before setThumbnailStore(), which is what hands the extractor
     * to the cache. Probing the binary costs one subprocess, done here rather than
     * per thumbnail.
     *
     * @param executableOverride From config.ini, or empty to search beside the
     *                           application and then PATH.
     * @return true when a usable binary was found. False is an ordinary outcome:
     *         those files then keep their placeholder, exactly as before step 17.
     */
    bool setFrameExtractor(const QString& executableOverride);

    /** Describes the ffmpeg binary in use, or empty when there is none. */
    QString frameExtractorVersion() const;

    /**
     * @brief Enables optimised video renditions.
     *
     * Requires setFrameExtractor() first, since it reuses the ffmpeg it found. Must
     * be called before start().
     *
     * @return true when renditions can be produced. False is ordinary: without
     *         ffprobe beside ffmpeg there is no way to tell a file that needs one
     *         from a file that does not, and originals are served instead.
     */
    bool setVideoStore(db::Database* database, const QString& cacheDirectory);

    /**
     * @brief The rendition store, or null when it was never enabled.
     *
     * Exposed so the desktop window can start the background pass and show what it
     * is doing.
     */
    media::VideoStore* videoStore() const { return m_videos.get(); }

    /**
     * @brief Enables background warming of the thumbnail cache.
     *
     * Requires setThumbnailStore() first. The scan starts with the server and stops
     * with it, and yields whenever a transfer is active.
     */
    void setThumbnailWarmupEnabled(bool enabled);
    bool thumbnailWarmupEnabled() const { return m_warmupEnabled; }

    /**
     * @brief Enables accounts, and requires one for every route that reads files.
     *
     * Must be called before start(). Installs handlers::AuthGate in front of the
     * file router, so after this call an anonymous request is redirected to the
     * login page rather than served.
     *
     * Without this call the server has no gate, and handlers::FileRouter answers
     * 401 to everything — refusing is the safe failure for a wiring mistake.
     */
    void setAccountStore(db::Database* database);

    /** Accounts on record, so the UI can tell whether anyone has been created. */
    int accountCount() const;

    /**
     * @brief The account store, or null when setAccountStore() was never called.
     *
     * Returned directly rather than wrapped in pass-through methods: creating and
     * listing accounts is the UI's business, and a facade over it would be four
     * forwarding functions that add nothing.
     */
    auth::UserStore* accountStore() const { return m_users.get(); }

    /**
     * @brief The audit log, or null when setAccountStore() was never called.
     *
     * Exposed for the same reason as accountStore(): creating an account is the one
     * audit-worthy thing that happens in the desktop window rather than over HTTP,
     * so the window has to be able to record it.
     */
    db::AuditLog* auditLog() const { return m_audit.get(); }

signals:
    void serverStarted(QString ip, quint16 port);
    void serverStopped();

private:
    std::unique_ptr<handlers::FileRouter>  m_router;
    std::unique_ptr<media::FrameExtractor>   m_frames;
    std::unique_ptr<media::VideoTranscoder>  m_transcoder;
    std::unique_ptr<media::VideoStore>       m_videos;
    std::unique_ptr<media::ThumbnailCache>   m_thumbnails;
    std::unique_ptr<media::ThumbnailScanner> m_scanner;

    std::unique_ptr<auth::UserStore>        m_users;
    std::unique_ptr<auth::SessionStore>     m_sessions;
    std::unique_ptr<auth::PermissionStore>  m_folderPerms;
    std::unique_ptr<auth::LoginRateLimiter> m_limiter;
    std::unique_ptr<db::AuditLog>           m_audit;
    std::unique_ptr<handlers::AuthHandler>  m_authentication;
    std::unique_ptr<handlers::AuthGate>     m_gate;

    /// Retires expired sessions while the server runs. Startup is handled by
    /// SessionStore's constructor; this covers a server left up for weeks.
    std::unique_ptr<QTimer> m_sessionSweeper;

    std::unique_ptr<http::HttpServer>      m_server;

    QSslConfiguration m_sslConfiguration;
    QString           m_tlsErrorString;
    bool              m_useHttps = false;
    quint16           m_port     = 9090;
    int               m_streamSlotOverride = 0;
    bool              m_warmupEnabled      = true;
};
