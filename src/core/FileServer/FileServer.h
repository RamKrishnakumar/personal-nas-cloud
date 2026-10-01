#pragma once

#include <QObject>
#include <QSslConfiguration>
#include <QString>

#include <memory>

namespace db       { class Database; }
namespace handlers { class FileRouter; }
namespace http     { class HttpServer; }
namespace media    { class ThumbnailCache; class ThumbnailScanner; }

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

    void setUseHttps(bool enabled) { m_useHttps = enabled; }
    bool useHttps() const { return m_useHttps; }

    QString tlsErrorString() const { return m_tlsErrorString; }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool    start(quint16 port = 9090);
    void    stop();
    bool    isRunning() const;
    quint16 port() const { return m_port; }
    QString localIP() const;

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
     * @brief Enables background warming of the thumbnail cache.
     *
     * Requires setThumbnailStore() first. The scan starts with the server and stops
     * with it, and yields whenever a transfer is active.
     */
    void setThumbnailWarmupEnabled(bool enabled);
    bool thumbnailWarmupEnabled() const { return m_warmupEnabled; }

signals:
    void serverStarted(QString ip, quint16 port);
    void serverStopped();

private:
    std::unique_ptr<handlers::FileRouter>  m_router;
    std::unique_ptr<media::ThumbnailCache>   m_thumbnails;
    std::unique_ptr<media::ThumbnailScanner> m_scanner;
    std::unique_ptr<http::HttpServer>      m_server;

    QSslConfiguration m_sslConfiguration;
    QString           m_tlsErrorString;
    bool              m_useHttps = false;
    quint16           m_port     = 9090;
    int               m_streamSlotOverride = 0;
    bool              m_warmupEnabled      = true;
};
