#include "FileServer.h"

#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>
#include <QDebug>
#include <QTimer>

/**
 * @class FileServer
 * @brief A production-ready HTTP file server using Qt.
 * * Features implemented:
 * 1. Non-blocking Chunked Streaming (Low RAM footprint).
 * 2. Automated MIME detection via QMimeDatabase.
 * 3. Security-focused path validation to prevent traversal attacks.
 * 4. Responsive HTML5/CSS3 web interface.
 */

FileServer::FileServer(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(8080)
{
    connect(m_server, &QTcpServer::newConnection,
            this,      &FileServer::onNewConnection);
}

bool FileServer::start(quint16 port)
{
    m_port = port;
    // Listen on all available IPv4 interfaces
    if (m_server->listen(QHostAddress::AnyIPv4, m_port)) {
        emit serverStarted(localIP(), m_port);
        return true;
    }
    qDebug() << "FileServer Critical Error:" << m_server->errorString();
    return false;
}

void FileServer::stop()
{
    m_server->close();
    // Force Windows to release the socket immediately
    // instead of waiting for TIME_WAIT timeout
    m_server->deleteLater();
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection,
            this, &FileServer::onNewConnection);
            
    emit serverStopped();
}

bool FileServer::isRunning() const { return m_server->isListening(); }
quint16 FileServer::port()     const { return m_port; }

QString FileServer::localIP() const
{
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol 
            && addr != QHostAddress::LocalHost)
            return addr.toString();
    }
    return "127.0.0.1";
}

void FileServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();

        // ← Removed Qt::UniqueConnection — doesn't work with lambdas
        connect(socket, &QTcpSocket::readyRead,
                this, [this, socket]() {
                    handleRequest(socket);
                });

        connect(socket, &QTcpSocket::disconnected,
                socket, &QTcpSocket::deleteLater);
    }
}

// ── Production Helpers ────────────────────────────────────────────────────────

/**
 * @brief Uses the system's MIME database to determine file type.
 */
static QString getMimeType(const QString& fsPath)
{
    QMimeDatabase db;
    QMimeType type = db.mimeTypeForFile(fsPath);
    return type.name(); // e.g., "video/mp4" or "image/png"
}

// ── HTTP Response Handlers ───────────────────────────────────────────────────

static void sendHTML(QTcpSocket* s, const QString& html)
{
    QByteArray body = html.toUtf8();
    s->write("HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
             "Connection: close\r\n"
             "\r\n" + body);
    s->flush();
    s->disconnectFromHost();
}

/**
 * @brief High-Performance File Streaming
 * Uses 64KB chunks and flow-control to serve files without high RAM usage.
 */
static void sendFile(QTcpSocket* socket, const QString& fsPath, bool inline_)
{
    // ← Add this: disconnect readyRead to prevent request collision
    QObject::disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);

    QFile* file = new QFile(fsPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        socket->disconnectFromHost();
        return;
    }

    qint64 fileSize = file->size();
    QString mime    = getMimeType(fsPath);
    QString disp    = inline_
                       ? "inline"
                       : "attachment; filename=\"" + QFileInfo(fsPath).fileName() + "\"";

    socket->write("HTTP/1.1 200 OK\r\n"
                  "Content-Type: "        + mime.toUtf8()                  + "\r\n"
                                    "Content-Disposition: " + disp.toUtf8()                  + "\r\n"
                                    "Content-Length: "      + QByteArray::number(fileSize)   + "\r\n"
                                                   "Accept-Ranges: bytes\r\n"
                                                   "Connection: close\r\n"
                                                   "\r\n");

    auto streamData = [socket, file]() {
        const qint64 CHUNK_SIZE = 65536;
        while (socket->bytesToWrite() < CHUNK_SIZE && !file->atEnd()) {
            QByteArray chunk = file->read(CHUNK_SIZE);
            if (socket->write(chunk) == -1) {
                file->close();
                return;
            }
        }
        if (file->atEnd() && socket->bytesToWrite() == 0) {
            socket->disconnectFromHost();
        }
    };

    QObject::connect(socket, &QTcpSocket::bytesWritten, streamData);
    QObject::connect(socket, &QTcpSocket::disconnected, file, &QFile::deleteLater);

    streamData();
}


// ── Main Request Router ───────────────────────────────────────────────────────

void FileServer::handleRequest(QTcpSocket* socket)
{
    if (socket->bytesAvailable() == 0) return;

    QByteArray data = socket->readAll();
    QString request(data);
    QString urlPath = "/";

    // Basic HTTP Header Parsing
    QStringList lines = request.split("\r\n");
    if (!lines.isEmpty()) {
        QStringList parts = lines[0].split(" ");
        if (parts.size() >= 2)
            urlPath = QUrl::fromPercentEncoding(parts[1].toUtf8());
    }

    emit requestReceived(urlPath);

    // Parse Query Parameters
    bool forceDownload = urlPath.contains("?download=1");
    urlPath = urlPath.split("?").first();

    // Security: Clean and Validate Path
    QString fsPath = QDir::cleanPath(nasRoot + urlPath);
    if (!fsPath.startsWith(nasRoot)) {
        QByteArray b = "<h1>403 Forbidden</h1>";
        socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: " 
                      + QByteArray::number(b.size()) + "\r\n\r\n" + b);
        socket->disconnectFromHost();
        return;
    }

    QFileInfo info(fsPath);
    if (!info.exists()) {
        QByteArray b = "<h1>404 Not Found</h1>";
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: " 
                      + QByteArray::number(b.size()) + "\r\n\r\n" + b);
        socket->disconnectFromHost();
        return;
    }

    // Routing Logic
    if (info.isDir()) {
        sendHTML(socket, buildFileListHTML(fsPath, urlPath));
    } else {
        QString mime = getMimeType(fsPath);
        
        if (forceDownload) {
            sendFile(socket, fsPath, false);
        } else if (mime.startsWith("image/")) {
            sendHTML(socket, buildPreviewHTML(urlPath, info.fileName(), "image"));
        } else if (mime.startsWith("video/")) {
            sendHTML(socket, buildPreviewHTML(urlPath, info.fileName(), "video"));
        } else {
            // Raw binary for files that aren't previewable
            sendFile(socket, fsPath, false);
        }
    }
}

// ── UI Building (HTML/CSS) ────────────────────────────────────────────────────

QString FileServer::buildFileListHTML(const QString& dirPath, const QString& urlPath)
{
    QDir dir(dirPath);
    QString html = 
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>NestDrive</title><style>"
        "body{margin:0;font-family:-apple-system,sans-serif;background:#0f0f1a;color:#ddd;}"
        "header{background:#111;padding:20px;border-bottom:1px solid #222;}"
        "header h1{margin:0;font-size:1.2rem;color:#00d4ff;} header span{color:#666;font-size:0.8rem;}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:15px;padding:20px;}"
        ".card{background:#1a1a2e;border-radius:10px;overflow:hidden;transition:0.2s;border:1px solid #252545;}"
        ".card:hover{transform:translateY(-5px);border-color:#00d4ff;}"
        ".thumb-icon{height:120px;display:flex;align-items:center;justify-content:center;font-size:3rem;background:#161625;}"
        ".thumb-img{width:100%;height:120px;object-fit:cover;}"
        ".info{padding:10px;font-size:0.8rem;text-align:center;}"
        ".back{display:inline-block;margin:15px 20px;color:#00d4ff;text-decoration:none;font-weight:bold;}"
        "a{text-decoration:none;color:inherit;}"
        "</style></head><body><header><h1>📁 NestDrive</h1><span>" + urlPath + "</span></header>";

    if (urlPath != "/") {
        QString parent = urlPath.left(urlPath.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        html += "<a class='back' href='" + parent + "'>&larr; Back to Parent</a>";
    }

    html += "<div class='grid'>";
    QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);

    for (const QFileInfo& entry : entries) {
        QString name = entry.fileName();
        QString link = (urlPath.endsWith("/") ? urlPath : urlPath + "/") + name;
        QString mime = getMimeType(entry.absoluteFilePath());

        html += "<a href='" + link + "'><div class='card'>";
        
        if (entry.isDir()) {
            html += "<div class='thumb-icon'>📁</div>";
        } else if (mime.startsWith("image/")) {
            html += "<img class='thumb-img' src='" + link + "?download=1' loading='lazy'/>";
        } else if (mime.startsWith("video/")) {
            html += "<div class='thumb-icon'>🎬</div>";
        } else {
            html += "<div class='thumb-icon'>📄</div>";
        }

        QString sizeLabel = entry.isDir() ? "Folder" : QString::number(entry.size() / 1024) + " KB";
        html += "<div class='info'><b>" + name + "</b><br><span style='color:#666'>" + sizeLabel + "</span></div></div></a>";
    }

    html += "</div></body></html>";
    return html;
}

QString FileServer::buildPreviewHTML(const QString& urlPath, const QString& fileName, const QString& type)
{
    QString mediaTag;
    if (type == "image") {
        mediaTag = "<img src='" + urlPath + "?download=1' style='max-width:100%;max-height:75vh;box-shadow:0 10px 30px rgba(0,0,0,0.5);border-radius:8px;'/>";
    } else {
        mediaTag = "<video controls autoplay style='max-width:100%;max-height:75vh;border-radius:8px;'>"
                   "<source src='" + urlPath + "?download=1'>Browser not supported.</video>";
    }

    QString parent = urlPath.left(urlPath.lastIndexOf('/'));
    if (parent.isEmpty()) parent = "/";

    return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Preview</title><style>"
           "body{margin:0;background:#05050a;color:#eee;font-family:sans-serif;display:flex;flex-direction:column;align-items:center;padding:40px;}"
           "h2{margin-bottom:20px;font-weight:300;color:#00d4ff;word-break:break-all;text-align:center;}"
           ".btns{margin-top:30px;display:flex;gap:15px;}"
           ".btn{padding:12px 25px;border-radius:30px;text-decoration:none;font-size:0.9rem;font-weight:bold;transition:0.2s;}"
           ".btn-back{background:#222;color:#ccc;} .btn-back:hover{background:#333;}"
           ".btn-dl{background:#00d4ff;color:#000;} .btn-dl:hover{background:#00b8e6;}"
           "</style></head><body>"
           "<h2>" + fileName + "</h2>" + mediaTag +
           "<div class='btns'>"
           "<a href='" + parent + "' class='btn btn-back'>Return</a>"
           "<a href='" + urlPath + "?download=1' download class='btn btn-dl'>Download File</a>"
           "</div></body></html>";
}