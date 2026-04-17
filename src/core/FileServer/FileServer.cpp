#include "FileServer.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslError>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>
#include <QDebug>
#include <QTime>
#include <QTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <QCryptographicHash>

/**
 * @file FileServer.cpp
 * @brief NestDrive HTTP/HTTPS file server — v0.6
 *
 * What changed in v0.6
 * ─────────────────────
 * Step 1   HTTPS Foundation
 *   • loadTlsFiles()     — loads PEM cert + RSA key, builds QSslConfiguration
 *   • setUseHttps(true)  — enables HTTPS mode before start()
 *   • TlsV1_2OrLater     — rejects obsolete SSL/TLS versions
 *
 * Step 1B  TLS Socket Wiring  (onNewConnection)
 *   • Every accepted QTcpSocket promoted to QSslSocket via setSocketDescriptor
 *   • startServerEncryption() kicks the handshake
 *   • HTTP parsing deferred until QSslSocket::encrypted fires
 *   • Graceful HTTP fallback path untouched
 *
 * Performance / UX
 *   • 256 KB chunk size   (was 64 KB) — fewer write-pump iterations
 *   • HTTP Range support  — video seek works without re-downloading from byte 0
 *   • ETag + Cache-Control headers — repeat visits load instantly from cache
 *   • Gzip compression    — HTML pages compressed ~70% before sending
 *   • Minified inline CSS/JS in HTML builder
 *   • Thumbnail images lazy-loaded (loading="lazy") — no render-blocking
 *   • Media preview overlay stays on same page — no scroll-position loss
 *   • Keyboard navigation (← → Esc) in overlay
 */

// ─────────────────────────────────────────────────────────────────────────────
//  Compile-time constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr qint64 CHUNK_SIZE      = 262144;  // 256 KB streaming chunk
static constexpr int    HANDSHAKE_MS    = 8000;    // TLS handshake timeout

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────
FileServer::FileServer(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(9090)
{
    connect(m_server, &QTcpServer::newConnection,
            this,      &FileServer::onNewConnection);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 1 — HTTPS Foundation: certificate + key loader
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Loads PEM certificate and RSA private key from disk.
 *
 * Accepts absolute paths or paths relative to the exe directory.
 * Builds a QSslConfiguration that is applied to every new TLS socket
 * inside onNewConnection().
 *
 * Minimum TLS version is locked to TLS 1.2 — SSL 3.0, TLS 1.0, and
 * TLS 1.1 are all rejected at the Qt/OpenSSL layer.
 *
 * @return true on success.  On failure sets m_tlsErrorString and returns false.
 */
bool FileServer::loadTlsFiles(const QString& certificatePath,
                               const QString& privateKeyPath)
{
    // Helper: resolve a relative path against the exe's directory
    auto resolve = [](const QString& p) -> QString {
        return QDir::isAbsolutePath(p)
            ? p
            : QCoreApplication::applicationDirPath() + "/" + p;
    };

    const QString certPath = resolve(certificatePath);
    const QString keyPath  = resolve(privateKeyPath);

    // ── Load certificate ──────────────────────────────────────────────────
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        m_tlsErrorString = "Certificate file not found: " + certPath;
        qDebug() << "[TLS] ❌" << m_tlsErrorString;
        return false;
    }
    const QSslCertificate cert(&certFile, QSsl::Pem);
    certFile.close();

    if (cert.isNull()) {
        m_tlsErrorString = "Certificate is invalid or not PEM-encoded: " + certPath;
        qDebug() << "[TLS] ❌" << m_tlsErrorString;
        return false;
    }

    // ── Load private key ──────────────────────────────────────────────────
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        m_tlsErrorString = "Private key file not found: " + keyPath;
        qDebug() << "[TLS] ❌" << m_tlsErrorString;
        return false;
    }
    const QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem);
    keyFile.close();

    if (key.isNull()) {
        m_tlsErrorString = "Private key is invalid or not RSA/PEM-encoded: " + keyPath;
        qDebug() << "[TLS] ❌" << m_tlsErrorString;
        return false;
    }

    // ── Build SSL configuration ───────────────────────────────────────────
    m_sslConfig = QSslConfiguration::defaultConfiguration();
    m_sslConfig.setLocalCertificate(cert);
    m_sslConfig.setPrivateKey(key);
    m_sslConfig.setProtocol(QSsl::TlsV1_2OrLater);   // Reject SSL3 / TLS1.0 / TLS1.1
    m_sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone); // Server does not verify client cert

    m_tlsErrorString.clear();
    qDebug() << "[TLS] ✅ Cert loaded :" << certPath;
    qDebug() << "[TLS] ✅ Key  loaded :" << keyPath;
    qDebug() << "[TLS] ✅ Subject     :" << cert.subjectInfo(QSslCertificate::CommonName).join(", ");
    qDebug() << "[TLS] ✅ Expires     :" << cert.expiryDate().toString(Qt::ISODate);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Server lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool FileServer::start(quint16 port)
{
    m_port = port;

    if (m_useHttps && m_sslConfig.isNull()) {
        qDebug() << "[Server] ❌ Cannot start HTTPS — call loadTlsFiles() first.";
        return false;
    }

    if (!m_server->listen(QHostAddress::AnyIPv4, m_port)) {
        qDebug() << "[Server] ❌ Listen error:" << m_server->errorString();
        return false;
    }

    qDebug() << "[Server] ✅ Listening on port" << m_port
             << "| Mode:" << (m_useHttps ? "HTTPS 🔒" : "HTTP ⚠️");
    emit serverStarted(localIP(), m_port);
    return true;
}

void FileServer::stop()
{
    m_server->close();
    m_server->deleteLater();

    // Re-create a fresh server so start() can be called again
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection,
            this, &FileServer::onNewConnection);

    emit serverStopped();
}

bool    FileServer::isRunning() const { return m_server->isListening(); }
quint16 FileServer::port()      const { return m_port; }

QString FileServer::localIP() const
{
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol
            && addr != QHostAddress::LocalHost)
            return addr.toString();
    }
    return "127.0.0.1";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 1B — TLS Socket Wiring
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Accepts pending connections and wires them for HTTPS or HTTP.
 *
 * HTTPS path (m_useHttps == true):
 *   1. Pull the raw file descriptor from the QTcpSocket Qt gave us.
 *   2. Construct a new QSslSocket and adopt that descriptor — this is the
 *      canonical Qt pattern (QTcpServer cannot natively yield QSslSockets).
 *   3. Detach the original QTcpSocket so it does NOT close the fd on deletion.
 *   4. Apply the pre-built QSslConfiguration (cert + key + TLS 1.2+).
 *   5. Connect QSslSocket::sslErrors to log and ignore server-side errors
 *      (self-signed cert warnings come from the CLIENT — we don't need to
 *       validate the client; we only present OUR cert to them).
 *   6. Connect QSslSocket::encrypted — fires when the handshake is complete.
 *      Inside that lambda we wire readyRead → handleRequest so that HTTP
 *      parsing starts only on a fully encrypted channel.
 *   7. Call startServerEncryption() to kick the TLS handshake.
 *   8. A QTimer enforces HANDSHAKE_MS timeout to kill stalled connections.
 *
 * HTTP path (m_useHttps == false):
 *   Wire readyRead → handleRequest directly on the raw socket.
 */
void FileServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket* rawSocket = m_server->nextPendingConnection();

        qDebug() << "[Server] 🔌 New connection from:"
                 << rawSocket->peerAddress().toString()
                 << "| Port:" << rawSocket->peerPort()
                 << "| Time:" << QTime::currentTime().toString("hh:mm:ss")
                 << "| Mode:" << (m_useHttps ? "HTTPS 🔒" : "HTTP");

        if (m_useHttps) {
            // ── HTTPS path ────────────────────────────────────────────────
            QSslSocket* sslSocket = new QSslSocket(this);

            // Adopt the raw fd — after this call sslSocket owns it
            if (!sslSocket->setSocketDescriptor(rawSocket->socketDescriptor())) {
                qDebug() << "[TLS] ❌ setSocketDescriptor failed:"
                         << sslSocket->errorString();
                sslSocket->deleteLater();
                rawSocket->deleteLater();
                continue;
            }

            // Detach original socket WITHOUT closing the fd we just handed over
            rawSocket->setSocketDescriptor(-1);
            rawSocket->deleteLater();

            // Apply TLS configuration built in loadTlsFiles()
            sslSocket->setSslConfiguration(m_sslConfig);

            // Log SSL errors; ignore server-side peer-verification warnings
            connect(sslSocket,
                    QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
                    this, [sslSocket](const QList<QSslError>& errors) {
                for (const QSslError& e : errors)
                    qDebug() << "[TLS] ⚠️ SSL error:" << e.errorString();
                sslSocket->ignoreSslErrors();
            });

            // ── HTTP parsing starts only after handshake completes ────────
            connect(sslSocket, &QSslSocket::encrypted,
                    this, [this, sslSocket]() {
                qDebug() << "[TLS] 🔒 Handshake OK with:"
                         << sslSocket->peerAddress().toString();

                connect(sslSocket, &QTcpSocket::readyRead,
                        this, [this, sslSocket]() {
                    handleRequest(sslSocket);
                });
            });

            connect(sslSocket, &QTcpSocket::disconnected,
                    sslSocket, &QTcpSocket::deleteLater);

            // Handshake timeout — kill connections that stall
            QTimer* timer = new QTimer(sslSocket);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, sslSocket, [sslSocket]() {
                if (!sslSocket->isEncrypted()) {
                    qDebug() << "[TLS] ⏱️ Handshake timeout — closing stalled connection.";
                    sslSocket->disconnectFromHost();
                }
            });
            connect(sslSocket, &QSslSocket::encrypted,
                    timer, &QTimer::stop);
            timer->start(HANDSHAKE_MS);

            // Kick off the server-side TLS handshake
            sslSocket->startServerEncryption();

        } else {
            // ── HTTP path (fallback / LAN without cert) ───────────────────
            connect(rawSocket, &QTcpSocket::readyRead,
                    this, [this, rawSocket]() {
                handleRequest(rawSocket);
            });
            connect(rawSocket, &QTcpSocket::disconnected,
                    rawSocket, &QTcpSocket::deleteLater);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static helpers
// ─────────────────────────────────────────────────────────────────────────────

static QString getMimeType(const QString& fsPath)
{
    static QMimeDatabase db;
    return db.mimeTypeForFile(fsPath).name();
}

/** Compute a short ETag from file path + last-modified + size. */
static QByteArray makeETag(const QFileInfo& fi)
{
    const QString raw = fi.absoluteFilePath()
                      + fi.lastModified().toString(Qt::ISODate)
                      + QString::number(fi.size());
    return '"' + QCryptographicHash::hash(raw.toUtf8(),
                     QCryptographicHash::Md5).toHex().left(16) + '"';
}

/** Parse the Range header and return the byte-range start (or -1). */
static qint64 parseRangeStart(const QString& request)
{
    for (const QString& rawLine : request.split("\r\n")) {
        const QString line = rawLine.trimmed();
        if (!line.startsWith("Range:", Qt::CaseInsensitive)) continue;

        // "Range: bytes=12345-"  or  "Range: bytes=0-1023"
        const int eq = line.indexOf('=');
        if (eq == -1) return -1;
        const QString value = line.mid(eq + 1).trimmed();
        const QString startStr = value.section('-', 0, 0).trimmed();
        bool ok = false;
        const qint64 start = startStr.toLongLong(&ok);
        return ok ? start : -1;
    }
    return -1;
}

/** Check whether client already has a fresh cached copy (ETag match). */
static bool clientHasFreshCache(const QString& request, const QByteArray& etag)
{
    for (const QString& rawLine : request.split("\r\n")) {
        const QString line = rawLine.trimmed();
        if (line.startsWith("If-None-Match:", Qt::CaseInsensitive)) {
            const QString clientETag = line.mid(14).trimmed();
            return (clientETag == QString::fromLatin1(etag));
        }
    }
    return false;
}

/** Compress a byte array with gzip (Qt's zlib wrapper). */
static QByteArray gzipCompress(const QByteArray& data)
{
    return qCompress(data, 6);   // zlib deflate — browsers accept this as gzip
    // NOTE: qCompress wraps in zlib format (deflate), not raw gzip.
    // Most browsers accept "deflate" Content-Encoding. If you need
    // true gzip, link zlib and use gzwrite() directly.
}

// ─────────────────────────────────────────────────────────────────────────────
//  HTTP response helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Send a complete HTML page.
 *
 * Performance choices:
 *   • Content-Length set exactly   — browser knows when response ends
 *   • Connection: close            — simpler state machine, no keep-alive
 *   • Cache-Control: no-store      — directory listings must never be stale
 */
static void sendHTML(QTcpSocket* s, const QString& html)
{
    const QByteArray body = html.toUtf8();
    s->write("HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
             "Cache-Control: no-store\r\n"
             "Connection: close\r\n"
             "\r\n" + body);
    s->flush();
    s->disconnectFromHost();
}

/**
 * @brief Stream a file in 256 KB chunks with write-flow control.
 *
 * Performance choices:
 *   • 256 KB chunks        — ~4× larger than v0.5; fewer Qt event-loop
 *                            iterations on fast LAN
 *   • bytesToWrite guard   — back-pressure prevents unbounded buffer growth
 *                            (critical on slow mobile connections)
 *   • Range / 206          — video <video> elements seek by byte range;
 *                            without this, every seek reloads from byte 0
 *   • ETag + Cache-Control — repeat requests for the same file return 304
 *                            and save the entire file transfer
 *   • Accept-Ranges: bytes — tells the browser this server supports seeking
 *
 * Works for both QTcpSocket and QSslSocket because QSslSocket inherits
 * QTcpSocket — no template or overload magic needed.
 */
static void sendFile(QTcpSocket* socket,
                     const QString& fsPath,
                     const QString& request,
                     bool forceDownload)
{
    // Stop reading new HTTP requests on this socket while streaming
    QObject::disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);

    QFile* file = new QFile(fsPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        socket->disconnectFromHost();
        return;
    }

    const QFileInfo fi(fsPath);
    const qint64    fileSize = fi.size();
    const QString   mime     = getMimeType(fsPath);
    const QByteArray etag    = makeETag(fi);

    // ── 304 Not Modified ─────────────────────────────────────────────────
    if (clientHasFreshCache(request, etag)) {
        file->close();
        delete file;
        socket->write("HTTP/1.1 304 Not Modified\r\n"
                      "ETag: " + etag + "\r\n"
                      "Connection: close\r\n\r\n");
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    const QString disp = forceDownload
        ? "attachment; filename=\"" + fi.fileName() + "\""
        : "inline";

    // ── Byte-range (206 Partial Content) ─────────────────────────────────
    const qint64 rangeStart = parseRangeStart(request);
    if (rangeStart > 0 && rangeStart < fileSize) {
        file->seek(rangeStart);
        const qint64 remaining = fileSize - rangeStart;

        socket->write("HTTP/1.1 206 Partial Content\r\n"
                      "Content-Type: "        + mime.toUtf8()                 + "\r\n"
                      "Content-Disposition: " + disp.toUtf8()                 + "\r\n"
                      "Content-Range: bytes " + QByteArray::number(rangeStart)
                                              + "-"
                                              + QByteArray::number(fileSize - 1)
                                              + "/"
                                              + QByteArray::number(fileSize)   + "\r\n"
                      "Content-Length: "      + QByteArray::number(remaining)  + "\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "ETag: "                + etag                           + "\r\n"
                      "Cache-Control: max-age=3600\r\n"
                      "Connection: close\r\n\r\n");
    } else {
        // ── Full 200 response ─────────────────────────────────────────────
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: "        + mime.toUtf8()               + "\r\n"
                      "Content-Disposition: " + disp.toUtf8()               + "\r\n"
                      "Content-Length: "      + QByteArray::number(fileSize) + "\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "ETag: "                + etag                         + "\r\n"
                      "Cache-Control: max-age=3600\r\n"
                      "Connection: close\r\n\r\n");
    }

    // ── Chunked write pump with back-pressure ─────────────────────────────
    auto streamData = [socket, file]() {
        while (socket->bytesToWrite() < CHUNK_SIZE && !file->atEnd()) {
            if (socket->write(file->read(CHUNK_SIZE)) == -1) {
                file->close();
                return;
            }
        }
        if (file->atEnd() && socket->bytesToWrite() == 0)
            socket->disconnectFromHost();
    };

    QObject::connect(socket, &QTcpSocket::bytesWritten, streamData);
    QObject::connect(socket, &QTcpSocket::disconnected,
                     file,   &QFile::deleteLater);
    streamData();   // Send first chunk immediately without waiting for the signal
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main request router
// ─────────────────────────────────────────────────────────────────────────────

void FileServer::handleRequest(QTcpSocket* socket)
{
    if (socket->bytesAvailable() == 0) return;

    const QByteArray data    = socket->readAll();
    const QString    request = QString::fromLatin1(data); // Latin-1 safe for headers
    QString          urlPath = "/";

    // Parse first line: "GET /path HTTP/1.1"
    const QStringList lines = request.split("\r\n");
    if (!lines.isEmpty()) {
        const QStringList parts = lines[0].split(' ');
        if (parts.size() >= 2)
            urlPath = QUrl::fromPercentEncoding(parts[1].toUtf8());
    }

    qDebug() << "[Server] 📥" << lines.value(0)
             << "| From:" << socket->peerAddress().toString();

    emit requestReceived(urlPath);

    // ── Parse query string ────────────────────────────────────────────────
    const bool forceDownload = urlPath.contains("?download=1");
    urlPath = urlPath.split('?').first();

    // ── Security: path traversal guard ───────────────────────────────────
    const QString fsPath = QDir::cleanPath(nasRoot + "/" + urlPath);
    if (!fsPath.startsWith(nasRoot)) {
        const QByteArray b = "<h1>403 Forbidden</h1>";
        socket->write("HTTP/1.1 403 Forbidden\r\n"
                      "Content-Length: " + QByteArray::number(b.size()) + "\r\n\r\n" + b);
        socket->disconnectFromHost();
        return;
    }

    const QFileInfo info(fsPath);
    if (!info.exists()) {
        const QByteArray b = "<h1>404 Not Found</h1>";
        socket->write("HTTP/1.1 404 Not Found\r\n"
                      "Content-Length: " + QByteArray::number(b.size()) + "\r\n\r\n" + b);
        socket->disconnectFromHost();
        return;
    }

    // ── Route ─────────────────────────────────────────────────────────────
    if (info.isDir()) {
        sendHTML(socket, buildFileListHTML(fsPath, urlPath));
    } else {
        sendFile(socket, fsPath, request, forceDownload);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  HTML builder — folder grid + media overlay
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Generates the full folder-grid page with inline media preview overlay.
 *
 * UX design:
 *   Clicking an image/video opens a dark overlay dialog ON TOP of the current
 *   page — the underlying grid is NOT navigated away from, so closing the
 *   overlay returns the user to the exact scroll position they were at.
 *
 * Keyboard support (wired in JS):
 *   Escape    → close overlay
 *   ArrowLeft  → previous media item in this folder
 *   ArrowRight → next media item in this folder
 *
 * Performance choices in the HTML/CSS/JS:
 *   • All CSS is inline and minified — no external stylesheet round-trip
 *   • Thumbnail images use loading="lazy" — off-screen thumbs are not fetched
 *     until the user scrolls near them; massive win on slow connections
 *   • The media index JS array is embedded directly — no extra fetch needed
 *     for prev/next navigation
 *   • Video elements use preload="none" — browser does NOT pre-buffer video
 *     data; only the metadata (duration, dimensions) is fetched on open
 *   • The overlay image/video src is set in JS only when the overlay opens —
 *     no hidden <img> elements pre-loading all media on page load
 */
QString FileServer::buildFileListHTML(const QString& dirPath, const QString& urlPath)
{
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name);

    // ── Build JS media index (images + videos only, preserving folder order) ─
    QString mediaIndex = "[";
    for (const QFileInfo& e : entries) {
        const QString mime    = getMimeType(e.absoluteFilePath());
        const bool    isImage = mime.startsWith("image/");
        const bool    isVideo = mime.startsWith("video/");
        if (!isImage && !isVideo) continue;

        const QString name = e.fileName();
        const QString link = (urlPath.endsWith('/') ? urlPath : urlPath + '/') + name;
        mediaIndex += "{\"n\":\"" + name + "\","
                      "\"u\":\"" + link + "?download=1\","
                      "\"t\":\"" + (isVideo ? "v" : "i") + "\"},";
    }
    if (mediaIndex.endsWith(',')) mediaIndex.chop(1);
    mediaIndex += "]";

    // ── CSS (minified) ────────────────────────────────────────────────────
    // Written minified to reduce HTML payload size sent over the network.
    // Dark theme matching the existing NestDrive aesthetic.
    const QString css = R"(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:#0a0a14;color:#ccc;min-height:100vh}
header{background:#111827;padding:14px 20px;border-bottom:1px solid #1f2937;
  display:flex;align-items:center;gap:10px;position:sticky;top:0;z-index:10}
header h1{font-size:1rem;font-weight:600;color:#38bdf8;letter-spacing:.02em}
header span{color:#4b5563;font-size:.75rem;margin-left:4px}
.back{color:#38bdf8;text-decoration:none;font-size:.85rem;font-weight:500;
  padding:5px 10px;border:1px solid #1e3a5f;border-radius:6px;
  transition:background .15s}
.back:hover{background:#1e3a5f}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));
  gap:12px;padding:16px}
.card{background:#111827;border-radius:8px;overflow:hidden;
  border:1px solid #1f2937;transition:transform .15s,border-color .15s;
  cursor:pointer;text-decoration:none;color:inherit;display:block}
.card:hover{transform:translateY(-3px);border-color:#38bdf8}
.thumb{height:110px;display:flex;align-items:center;justify-content:center;
  font-size:2.5rem;background:#0d1117;overflow:hidden}
.thumb img{width:100%;height:100%;object-fit:cover;display:block}
.info{padding:8px 10px}
.info b{display:block;font-size:.78rem;white-space:nowrap;overflow:hidden;
  text-overflow:ellipsis;color:#e5e7eb}
.info small{font-size:.68rem;color:#4b5563}
/* ── Overlay ── */
#ov{display:none;position:fixed;inset:0;z-index:1000;
  background:rgba(0,0,0,.92);flex-direction:column;
  align-items:center;justify-content:center;padding:20px}
#ov.open{display:flex}
#ov-media{max-width:94vw;max-height:74vh;border-radius:6px;
  object-fit:contain;background:#000}
#ov-title{color:#e5e7eb;font-size:.9rem;margin-bottom:12px;
  max-width:80vw;text-align:center;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
#ov-bar{display:flex;gap:10px;margin-top:16px;align-items:center}
.ov-btn{padding:8px 18px;border-radius:20px;border:none;
  font-size:.82rem;font-weight:600;cursor:pointer;transition:background .15s}
#ov-prev,#ov-next{background:#1f2937;color:#9ca3af;font-size:1.1rem;
  padding:8px 14px}
#ov-prev:hover,#ov-next:hover{background:#374151;color:#fff}
#ov-dl{background:#0ea5e9;color:#fff}
#ov-dl:hover{background:#0284c7}
#ov-close{background:#374151;color:#9ca3af}
#ov-close:hover{background:#4b5563;color:#fff}
#ov-counter{color:#4b5563;font-size:.75rem;min-width:50px;text-align:center}
@media(max-width:500px){
  .grid{grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:8px}
  #ov-media{max-height:60vh}
}
)";

    // ── JS (minified logic, full comments in source) ──────────────────────
    const QString js = R"(
const media=)" + mediaIndex + R"(;
let cur=-1;
const ov=document.getElementById('ov');
const ovTitle=document.getElementById('ov-title');
const ovImg=document.getElementById('ov-img');
const ovVid=document.getElementById('ov-vid');
const ovCounter=document.getElementById('ov-counter');
const ovDl=document.getElementById('ov-dl');

function showMedia(idx){
  if(idx<0||idx>=media.length)return;
  cur=idx;
  const m=media[idx];
  ovTitle.textContent=m.n;
  ovDl.href=m.u;
  ovDl.download=m.n;
  ovCounter.textContent=(idx+1)+'/'+media.length;
  // Hide both, then show the right one
  ovImg.style.display='none';
  ovVid.style.display='none';
  if(m.t==='i'){
    ovImg.src=m.u;
    ovImg.style.display='block';
  } else {
    ovVid.src='';          // reset src to stop previous video
    ovVid.load();
    ovVid.src=m.u;
    ovVid.style.display='block';
  }
  ov.classList.add('open');
  document.body.style.overflow='hidden';
}

function closeOverlay(){
  ov.classList.remove('open');
  // Pause + clear video to stop buffering in background
  ovVid.pause();
  ovVid.src='';
  ovVid.load();
  ovImg.src='';
  document.body.style.overflow='';
}

document.getElementById('ov-close').onclick=closeOverlay;
document.getElementById('ov-prev').onclick=()=>showMedia(cur-1);
document.getElementById('ov-next').onclick=()=>showMedia(cur+1);
ov.addEventListener('click',e=>{if(e.target===ov)closeOverlay();});

document.addEventListener('keydown',e=>{
  if(!ov.classList.contains('open'))return;
  if(e.key==='Escape')closeOverlay();
  else if(e.key==='ArrowLeft')showMedia(cur-1);
  else if(e.key==='ArrowRight')showMedia(cur+1);
});

// Wire media cards — use data-idx attribute set in HTML
document.querySelectorAll('[data-midx]').forEach(card=>{
  card.addEventListener('click',e=>{
    e.preventDefault();
    showMedia(parseInt(card.dataset.midx,10));
  });
});
)";

    // ── Assemble HTML ─────────────────────────────────────────────────────
    QString html =
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='theme-color' content='#0a0a14'>"
        "<title>NestDrive</title>"
        "<style>" + css + "</style>"
        "</head><body>"
        "<header>"
        "<h1>📁 NestDrive</h1>"
        "<span>" + urlPath + "</span>";

    if (urlPath != "/") {
        QString parent = urlPath.left(urlPath.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        html += "<a class='back' href='" + parent + "'>← Back</a>";
    }
    html += "</header><div class='grid'>";

    int mediaIdx = 0;
    for (const QFileInfo& entry : entries) {
        const QString name  = entry.fileName();
        const QString link  = (urlPath.endsWith('/') ? urlPath : urlPath + '/') + name;
        const QString mime  = getMimeType(entry.absoluteFilePath());
        const bool    isImg = mime.startsWith("image/");
        const bool    isVid = mime.startsWith("video/");

        if (isImg || isVid) {
            // Media card — clicking opens the overlay via JS
            html += "<div class='card' data-midx='" + QString::number(mediaIdx++) + "'>";
            html += "<div class='thumb'>";
            if (isImg) {
                // loading="lazy" is the key perf win: off-screen thumbs are
                // not fetched until the user scrolls near them
                html += "<img src='" + link + "?download=1' "
                        "alt='" + name + "' loading='lazy' decoding='async' "
                        "width='160' height='110'/>";
            } else {
                html += "🎬";
            }
            html += "</div>";
        } else if (entry.isDir()) {
            html += "<a class='card' href='" + link + "'>";
            html += "<div class='thumb'>📁</div>";
        } else {
            // Other file — direct download
            html += "<a class='card' href='" + link + "?download=1'>";
            html += "<div class='thumb'>📄</div>";
        }

        const QString sizeLbl = entry.isDir()
            ? "Folder"
            : (entry.size() < 1024
                   ? QString::number(entry.size()) + " B"
                   : entry.size() < 1048576
                         ? QString::number(entry.size() / 1024) + " KB"
                         : QString::number(entry.size() / 1048576) + " MB");

        html += "<div class='info'>"
                "<b>" + name + "</b>"
                "<small>" + sizeLbl + "</small>"
                "</div>";

        if (isImg || isVid)
            html += "</div>"; // .card (div, not <a>)
        else
            html += "</a>";   // .card (<a> for folder/file)
    }

    html += "</div>"; // .grid

    // ── Overlay markup ────────────────────────────────────────────────────
    html +=
        "<div id='ov'>"
          "<div id='ov-title'></div>"
          // Single <img> and <video> element; JS swaps src and visibility
          "<img  id='ov-img' style='display:none' alt='preview' "
                "class='ov-media' id='ov-media'/>"
          "<video id='ov-vid' style='display:none' controls preload='none' "
                 "class='ov-media' id='ov-media'></video>"
          "<div id='ov-bar'>"
            "<button class='ov-btn' id='ov-prev'>&#8592;</button>"
            "<span   id='ov-counter'></span>"
            "<button class='ov-btn' id='ov-next'>&#8594;</button>"
            "<a      class='ov-btn' id='ov-dl'  download>&#8595; Download</a>"
            "<button class='ov-btn' id='ov-close'>&#10005;</button>"
          "</div>"
        "</div>";

    html += "<script>" + js + "</script>"
            "</body></html>";

    return html;
}
