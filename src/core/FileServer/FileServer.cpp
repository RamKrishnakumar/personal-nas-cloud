
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
#include <QTime>
#include <QStringList>
#include <algorithm>

FileServer::FileServer(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(9090)
{
    connect(m_server, &QTcpServer::newConnection,
            this, &FileServer::onNewConnection);
}

bool FileServer::start(quint16 port)
{
    m_port = port;
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
    m_server->deleteLater();
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection,
            this, &FileServer::onNewConnection);

    emit serverStopped();
}

bool FileServer::isRunning() const { return m_server->isListening(); }
quint16 FileServer::port() const { return m_port; }

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

        qDebug() << "[Server] 🔌 New connection from:"
                 << socket->peerAddress().toString()
                 << "| Port:" << socket->peerPort()
                 << "| Time:" << QTime::currentTime().toString("hh:mm:ss");

        connect(socket, &QTcpSocket::readyRead,
                this, [this, socket]() {
                    handleRequest(socket);
                });

        connect(socket, &QTcpSocket::disconnected,
                socket, &QTcpSocket::deleteLater);
    }
}

static QString getMimeType(const QString& fsPath)
{
    QMimeDatabase db;
    QMimeType type = db.mimeTypeForFile(fsPath);
    return type.name();
}

static qint64 parseRangeStart(const QString& request)
{
    const QStringList lines = request.split("\r\n");
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (!line.startsWith("Range:", Qt::CaseInsensitive))
            continue;

        const int eq = line.indexOf('=');
        if (eq == -1)
            return -1;

        const QString value = line.mid(eq + 1).trimmed();
        if (!value.startsWith("bytes", Qt::CaseInsensitive))
            return -1;

        const QString rangePart = value.section('=', 1).trimmed();
        const QString firstRange = rangePart.section(',', 0, 0).trimmed();
        const QString startText = firstRange.section('-', 0, 0).trimmed();
        bool ok = false;
        const qint64 start = startText.toLongLong(&ok);
        return ok ? start : -1;
    }
    return -1;
}

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

static void sendFile(QTcpSocket* socket, const QString& fsPath, bool inline_, qint64 rangeStart = -1)
{
    QObject::disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);

    QFile* file = new QFile(fsPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        socket->disconnectFromHost();
        return;
    }

    const qint64 fileSize = file->size();
    const QString mime = getMimeType(fsPath);
    const QString disp = inline_
        ? "inline"
        : "attachment; filename=\"" + QFileInfo(fsPath).fileName() + "\"";

    qint64 startOffset = 0;
    qint64 bytesToSend = fileSize;
    QByteArray statusLine = "HTTP/1.1 200 OK\r\n";
    QByteArray extraHeaders;

    if (rangeStart >= 0 && rangeStart < fileSize) {
        startOffset = rangeStart;
        bytesToSend = fileSize - startOffset;
        file->seek(startOffset);
        statusLine = "HTTP/1.1 206 Partial Content\r\n";
        extraHeaders += "Content-Range: bytes "
                     + QByteArray::number(startOffset) + "-"
                     + QByteArray::number(fileSize - 1) + "/"
                     + QByteArray::number(fileSize) + "\r\n";
    } else if (rangeStart >= fileSize && fileSize > 0) {
        const QByteArray header = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                  "Content-Range: bytes */" + QByteArray::number(fileSize) + "\r\n"
                                  "Connection: close\r\n\r\n";
        socket->write(header);
        socket->disconnectFromHost();
        delete file;
        return;
    }

    socket->write(statusLine
                  + "Content-Type: " + mime.toUtf8() + "\r\n"
                  + "Content-Disposition: " + disp.toUtf8() + "\r\n"
                  + "Content-Length: " + QByteArray::number(bytesToSend) + "\r\n"
                  + "Accept-Ranges: bytes\r\n"
                  + extraHeaders
                  + "Connection: close\r\n"
                  + "\r\n");

    auto remaining = new qint64(bytesToSend);

    auto streamData = [socket, file, remaining]() {
        const qint64 CHUNK_SIZE = 65536;
        while (*remaining > 0 && socket->bytesToWrite() < CHUNK_SIZE && !file->atEnd()) {
            const qint64 nextSize = std::min(CHUNK_SIZE, *remaining);
            QByteArray chunk = file->read(nextSize);
            if (chunk.isEmpty()) {
                break;
            }
            const qint64 written = socket->write(chunk);
            if (written == -1) {
                file->close();
                return;
            }
            *remaining -= written;
        }
        if (*remaining <= 0 && socket->bytesToWrite() == 0) {
            socket->disconnectFromHost();
        }
    };

    QObject::connect(socket, &QTcpSocket::bytesWritten, socket, [streamData](qint64) {
        streamData();
    });
    QObject::connect(socket, &QTcpSocket::disconnected, file, &QFile::deleteLater);
    QObject::connect(socket, &QTcpSocket::disconnected, socket, [remaining]() {
        delete remaining;
    });

    streamData();
}

void FileServer::handleRequest(QTcpSocket* socket)
{
    if (socket->bytesAvailable() == 0) return;

    const QByteArray data = socket->readAll();
    const QString request = QString::fromUtf8(data);
    QString urlPath = "/";

    QStringList lines = request.split("\r\n");
    if (!lines.isEmpty()) {
        QStringList parts = lines[0].split(" ");
        if (parts.size() >= 2)
            urlPath = QUrl::fromPercentEncoding(parts[1].toUtf8());
    }

    qDebug() << "[Server] 📥 Request:" << lines.value(0)
             << "| From:" << socket->peerAddress().toString();

    emit requestReceived(urlPath);

    const bool forceDownload = urlPath.contains("?download=1");
    urlPath = urlPath.split("?").first();

    const QString fsPath = QDir::cleanPath(nasRoot + urlPath);
    if (!fsPath.startsWith(nasRoot)) {
        QByteArray b = "403 Forbidden";
        socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: "
                      + QByteArray::number(b.size()) + "\r\n\r\n" + b);
        socket->disconnectFromHost();
        return;
    }

    QFileInfo info(fsPath);
    if (!info.exists()) {
        QByteArray b = "404 Not Found";
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: "
                      + QByteArray::number(b.size()) + "\r\n\r\n" + b);
        socket->disconnectFromHost();
        return;
    }

    if (info.isDir()) {
        sendHTML(socket, buildFileListHTML(fsPath, urlPath));
    } else {
        const QString mime = getMimeType(fsPath);
        const qint64 rangeStart = parseRangeStart(request);

        if (forceDownload) {
            sendFile(socket, fsPath, false, rangeStart);
        } else if (mime.startsWith("image/") || mime.startsWith("video/")) {
            sendFile(socket, fsPath, true, rangeStart);
        } else {
            sendFile(socket, fsPath, false, rangeStart);
        }
    }
}

QString FileServer::buildFileListHTML(const QString& dirPath, const QString& urlPath)
{
    QDir dir(dirPath);
    QString html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>NestDrive</title><style>"
        ":root{--bg:#0b1020;--panel:#141b2d;--panel2:#1b2338;--text:#e8edf7;--muted:#8fa0bf;--accent:#4fd1ff;--border:#2b3654;}"
        "*{box-sizing:border-box}"
        "body{margin:0;font-family:Inter,Segoe UI,sans-serif;background:var(--bg);color:var(--text);}"
        "header{position:sticky;top:0;z-index:10;background:rgba(11,16,32,.9);backdrop-filter:blur(10px);padding:18px 20px;border-bottom:1px solid var(--border);}"
        "header h1{margin:0;font-size:1.2rem;color:var(--accent)}"
        "header span{display:block;margin-top:4px;color:var(--muted);font-size:.85rem;word-break:break-all}"
        ".back{display:inline-flex;align-items:center;gap:8px;margin:18px 20px 0;color:var(--accent);text-decoration:none;font-weight:600;}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:16px;padding:20px;}"
        ".card{cursor:pointer;position:relative;background:var(--panel);border:1px solid var(--border);border-radius:16px;overflow:hidden;transition:.18s ease;min-height:220px;}"
        ".card:hover{transform:translateY(-4px);border-color:var(--accent);box-shadow:0 16px 40px rgba(0,0,0,.28);}"
        ".thumb{height:138px;background:var(--panel2);display:flex;align-items:center;justify-content:center;font-size:3rem;position:relative;overflow:hidden;}"
        ".thumb img{width:100%;height:100%;object-fit:cover;display:block}"
        ".info{padding:12px 14px;}"
        ".name{font-weight:700;line-height:1.35;word-break:break-word;}"
        ".meta{margin-top:6px;color:var(--muted);font-size:.85rem}.preview-hint{margin-top:8px;display:inline-flex;align-items:center;gap:6px;padding:6px 10px;border-radius:999px;background:rgba(79,209,255,.12);color:var(--accent);font-size:.76rem;font-weight:700}.card.media-card .thumb::after{content:'Tap to preview';position:absolute;right:10px;bottom:10px;background:rgba(6,10,20,.78);color:#fff;border:1px solid rgba(255,255,255,.12);padding:6px 10px;border-radius:999px;font-size:.72rem;font-weight:700;letter-spacing:.01em}.card.media-card.video-card .thumb::after{content:'Play video'}"
        "a{text-decoration:none;color:inherit}"
        ".preview-overlay{position:fixed;inset:0;background:rgba(3,6,14,.82);backdrop-filter:blur(10px);display:none;align-items:center;justify-content:center;padding:20px;z-index:9999;}"
        ".preview-overlay.open{display:flex;}"
        ".preview-dialog{width:min(1000px,100%);height:min(86vh,860px);background:#0f1527;border:1px solid var(--border);border-radius:22px;overflow:hidden;display:grid;grid-template-rows:auto 1fr;box-shadow:0 30px 80px rgba(0,0,0,.45);}"
        ".preview-top{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 18px;border-bottom:1px solid var(--border);background:rgba(255,255,255,.02);}"
        ".preview-title{font-weight:700;max-width:70%;word-break:break-word}"
        ".preview-actions{display:flex;gap:10px;align-items:center;}"
        ".btn{border:1px solid var(--border);background:#18213a;color:var(--text);padding:10px 14px;border-radius:999px;font-weight:700;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;justify-content:center;min-width:44px;min-height:44px;}"
        ".btn:hover{border-color:var(--accent);color:var(--accent)}"
        ".btn-primary{background:var(--accent);color:#00131b;border-color:transparent;}"
        ".btn-primary:hover{filter:brightness(1.05);color:#00131b}"
        ".preview-body{display:flex;align-items:center;justify-content:center;padding:16px;overflow:auto;background:#090e1b;}"
        ".preview-stage{width:100%;height:100%;display:flex;align-items:center;justify-content:center;}"
        ".preview-stage img,.preview-stage video{max-width:100%;max-height:100%;border-radius:14px;box-shadow:0 12px 30px rgba(0,0,0,.35);background:#000;}"
        "@media (max-width:700px){.grid{grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px;padding:14px}.card{min-height:190px}.thumb{height:110px}.preview-dialog{height:min(92vh,900px);border-radius:18px}.preview-top{padding:12px}.preview-title{max-width:100%;font-size:.95rem}}"
        "</style></head><body>"
        "<header><h1>📁 NestDrive</h1><span>" + urlPath + "</span></header>";

    if (urlPath != "/") {
        QString parent = urlPath.left(urlPath.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        html += "<a class='back' href='" + parent + "'>&larr; Back to Parent</a>";
    }

    html += "<div class='grid'>";

    QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                              QDir::DirsFirst | QDir::Name);

    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QFileInfo& entry = *it;
        const QString name = entry.fileName();
        const QString link = (urlPath.endsWith("/") ? urlPath : urlPath + "/") + name;
        const QString mime = getMimeType(entry.absoluteFilePath());
        const bool isImage = mime.startsWith("image/");
        const bool isVideo = mime.startsWith("video/");

        QString cardClass = "card";
        if (isImage || isVideo) cardClass += " media-card";
        if (isVideo) cardClass += " video-card";

        html += "<a href='" + link + "'><div class='" + cardClass + "'>";

        if (entry.isDir()) {
            html += "<div class='thumb'>📁</div>";
        } else if (isImage) {
            html += "<div class='thumb'><img src='" + link + "?download=1' loading='lazy' alt='" + name + "'></div>";
        } else if (isVideo) {
            html += "<div class='thumb'>🎬</div>";
        } else {
            html += "<div class='thumb'>📄</div>";
        }

        const QString sizeLabel = entry.isDir() ? "Folder" : QString::number(entry.size() / 1024) + " KB";
        html += "<div class='info'><div class='name'>" + name + "</div><div class='meta'>" + sizeLabel + "</div>";
        html += "</div></div></a>";
    }

    html += "</div>"
            "<div id='previewOverlay' class='preview-overlay' aria-hidden='true'>"
            "<div class='preview-dialog' role='dialog' aria-modal='true' aria-label='Media preview'>"
            "<div class='preview-top'>"
            "<div id='previewTitle' class='preview-title'>Preview</div>"
            "<div class='preview-actions'>"
            "<a id='downloadBtn' class='btn btn-primary' href='#' download>Download</a>"
            "<button id='closeBtn' class='btn' type='button' aria-label='Close preview'>✕</button>"
            "</div></div>"
            "<div class='preview-body'><div id='previewStage' class='preview-stage'></div></div>"
            "</div></div>"
            "<script>"
            "const overlay=document.getElementById('previewOverlay');"
            "const stage=document.getElementById('previewStage');"
            "const titleEl=document.getElementById('previewTitle');"
            "const downloadBtn=document.getElementById('downloadBtn');"
            "const closeBtn=document.getElementById('closeBtn');"
            "function openPreview(name,url,type){stage.innerHTML='';titleEl.textContent=name;downloadBtn.href=url;downloadBtn.setAttribute('download',name);if(type==='image'){const img=document.createElement('img');img.src=url;img.alt=name;stage.appendChild(img);}else{const video=document.createElement('video');video.src=url;video.controls=true;video.autoplay=true;video.playsInline=true;video.preload='none';stage.appendChild(video);}overlay.classList.add('open');overlay.setAttribute('aria-hidden','false');document.body.style.overflow='hidden';}"
            "function closePreview(){overlay.classList.remove('open');overlay.setAttribute('aria-hidden','true');document.body.style.overflow='';stage.innerHTML='';}"
            "document.addEventListener('click',function(e){const anchor=e.target.closest('a');if(!anchor)return;const href=anchor.getAttribute('href');if(!href)return;const card=anchor.querySelector('.card');if(!card||(!card.classList.contains('media-card')))return;e.preventDefault();const nameEl=anchor.querySelector('.name');const isVideo=card.classList.contains('video-card');openPreview(nameEl?nameEl.textContent.trim():'Preview',href+'?download=1',isVideo?'video':'image');});"
            "closeBtn.addEventListener('click',closePreview);"
            "overlay.addEventListener('click',function(e){if(e.target===overlay)closePreview();});"
            "document.addEventListener('keydown',function(e){if(!overlay.classList.contains('open'))return;if(e.key==='Escape'){closePreview();}});"
            "</script></body></html>";

    return html;
}

QString FileServer::buildPreviewHTML(const QString& urlPath, const QString& fileName, const QString& type)
{
    Q_UNUSED(fileName);
    Q_UNUSED(type);

    QString parent = urlPath.left(urlPath.lastIndexOf('/'));
    if (parent.isEmpty()) parent = "/";

    return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Preview</title></head><body style='font-family:sans-serif;background:#0b1020;color:#fff;display:flex;align-items:center;justify-content:center;min-height:100vh;flex-direction:column;gap:16px;padding:20px'>"
           "<p>This file is meant to open in the gallery popup from the folder view.</p>"
           "<a href='" + parent + "' style='color:#4fd1ff'>Return to folder</a>"
           "</body></html>";
}
