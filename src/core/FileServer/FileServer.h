#pragma once

#include <QObject>
#include <QSslConfiguration>
#include <QString>

#include <memory>

namespace handlers { class FileRouter; }
namespace http     { class HttpServer; }

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

    /** Root directory exposed to clients. Set before start(). */
    void setNasRoot(const QString& path);

signals:
    void serverStarted(QString ip, quint16 port);
    void serverStopped();

private:
    std::unique_ptr<handlers::FileRouter> m_router;
    std::unique_ptr<http::HttpServer>     m_server;

    QSslConfiguration m_sslConfiguration;
    QString           m_tlsErrorString;
    bool              m_useHttps = false;
    quint16           m_port     = 9090;
};
