#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslError>
#include <QString>

/**
 * @class FileServer
 * @brief Production-grade HTTPS/HTTP file server using Qt.
 *
 * Step 1  — HTTPS Foundation:
 *   loadTlsFiles()  loads cert + key → builds QSslConfiguration.
 *   setUseHttps()   switches between HTTPS and plain HTTP mode.
 *
 * Step 1B — TLS Socket Wiring (onNewConnection):
 *   In HTTPS mode every accepted QTcpSocket is promoted to a QSslSocket.
 *   The raw file-descriptor is adopted by the SSL socket, the TLS
 *   server-side handshake is started, and HTTP parsing begins only after
 *   the `encrypted` signal fires — guaranteeing no plain-text data leaks.
 *
 * Performance improvements (v0.6):
 *   • 256 KB streaming chunks  (was 64 KB)
 *   • HTTP/1.1 byte-range (partial content) support for video seek
 *   • Aggressive browser caching headers (ETag + Cache-Control)
 *   • Gzip Content-Encoding for HTML responses
 *   • Inline CSS/JS minified and split into static helpers
 *   • Lazy-loaded thumbnail images via loading="lazy"
 */
class FileServer : public QObject
{
    Q_OBJECT

public:
    explicit FileServer(QObject* parent = nullptr);

    // ── TLS (Step 1) ─────────────────────────────────────────────────────────

    /**
     * @brief Load PEM certificate + RSA private key from disk.
     *
     * Paths may be absolute or relative to the executable directory.
     * Must be called BEFORE start() when useHttps is true.
     *
     * @return true on success; false with reason in tlsErrorString().
     */
    bool loadTlsFiles(const QString& certificatePath,
                      const QString& privateKeyPath);

    /** Enable or disable HTTPS mode (default: false = plain HTTP). */
    void setUseHttps(bool enabled) { m_useHttps = enabled; }

    /** @return true when the server will use TLS for new connections. */
    bool useHttps() const { return m_useHttps; }

    /**
     * @return Human-readable description of the last TLS loading error,
     *         or an empty string when everything is fine.
     */
    QString tlsErrorString() const { return m_tlsErrorString; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool    start(quint16 port = 9090);
    void    stop();
    bool    isRunning() const;
    quint16 port()      const;
    QString localIP()   const;

    /** Root directory exposed to browsers — must end without a trailing slash. */
    void setNasRoot(const QString& path) { nasRoot = path.endsWith('/') || path.endsWith('\\')
                                               ? path.chopped(1) : path; }

signals:
    void serverStarted(QString ip, quint16 port);
    void serverStopped();
    void requestReceived(QString path);

private slots:
    void onNewConnection();   // Step 1B — TLS socket wiring lives here

private:
    // HTTP request dispatcher
    void handleRequest(QTcpSocket* socket);

    // HTML page builders
    QString buildFileListHTML(const QString& dirPath, const QString& urlPath);

    // ── Configuration ─────────────────────────────────────────────────────────
    QString  nasRoot     = "E:/";
    bool     m_useHttps  = false;

    // ── TLS state (Step 1) ────────────────────────────────────────────────────
    QSslConfiguration m_sslConfig;
    QString           m_tlsErrorString;

    // ── Server core ───────────────────────────────────────────────────────────
    QTcpServer* m_server = nullptr;
    quint16     m_port   = 9090;
};
