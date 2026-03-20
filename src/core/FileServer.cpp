#include "FileServer.h"

#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QDateTime>
#include <QUrl>
#include <QDebug>

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
    if (m_server->listen(QHostAddress::AnyIPv4, m_port)) {
        emit serverStarted(localIP(), m_port);
        qDebug() << "FileServer started on" << localIP() << ":" << m_port;
        return true;
    }
    qDebug() << "FileServer failed:" << m_server->errorString();
    return false;
}

void FileServer::stop()
{
    m_server->close();
    emit serverStopped();
}

bool FileServer::isRunning() const
{
    return m_server->isListening();
}

quint16 FileServer::port() const
{
    return m_port;
}

QString FileServer::localIP() const
{
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol
            && addr != QHostAddress::LocalHost) {
            return addr.toString();
        }
    }
    return "127.0.0.1";
}

void FileServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleRequest(socket);
        });
        connect(socket, &QTcpSocket::disconnected,
                socket,  &QTcpSocket::deleteLater);
    }
}

void FileServer::handleRequest(QTcpSocket* socket)
{
    QByteArray requestData = socket->readAll();
    QString request(requestData);

    // Parse GET path from HTTP request
    QString urlPath = "/";
    QStringList lines = request.split("\r\n");
    if (!lines.isEmpty()) {
        QStringList parts = lines[0].split(" ");
        if (parts.size() >= 2)
            urlPath = QUrl::fromPercentEncoding(parts[1].toUtf8());
    }

    emit requestReceived(urlPath);

    // Map URL path to filesystem path
    QString fsPath = nasRoot + urlPath;
    fsPath = QDir::cleanPath(fsPath);

    // Security: prevent path traversal outside nasRoot
    if (!fsPath.startsWith(nasRoot)) {
        QByteArray body = "<h1>403 Forbidden</h1>";
        socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    QFileInfo info(fsPath);

    if (info.isDir()) {
        // Serve directory listing
        QString html = buildFileListHTML(fsPath, urlPath);
        QByteArray body = html.toUtf8();
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                      "\r\n" + body);
    } else if (info.isFile()) {
        // Serve file download
        QFile file(fsPath);
        if (file.open(QIODevice::ReadOnly)) {
            QMimeDatabase mimeDb;
            QString mimeType = mimeDb.mimeTypeForFile(fsPath).name();
            QByteArray fileData = file.readAll();
            socket->write("HTTP/1.1 200 OK\r\n"
                          "Content-Type: " + mimeType.toUtf8() + "\r\n"
                          "Content-Disposition: attachment; filename=\""
                          + info.fileName().toUtf8() + "\"\r\n"
                          "Content-Length: " + QByteArray::number(fileData.size()) + "\r\n"
                          "\r\n" + fileData);
            file.close();
        }
    } else {
        QByteArray body = "<h1>404 Not Found</h1>";
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    }

    socket->flush();
    socket->disconnectFromHost();
}

QString FileServer::buildFileListHTML(const QString& dirPath, const QString& urlPath)
{
    QDir dir(dirPath);
    QString html = "<!DOCTYPE html><html><head>"
                   "<meta charset='utf-8'>"
                   "<title>MyNAS - " + urlPath + "</title>"
                   "<style>"
                   "body{font-family:sans-serif;padding:20px;background:#1a1a2e;color:#eee;}"
                   "h2{color:#00d4ff;}"
                   "a{color:#00d4ff;text-decoration:none;display:block;padding:6px 0;}"
                   "a:hover{color:#fff;}"
                   ".item{padding:8px;border-bottom:1px solid #333;}"
                   ".folder{color:#ffd700;}"
                   "</style></head><body>"
                   "<h2>📁 MyNAS — " + urlPath + "</h2>";

    // Back button
    if (urlPath != "/") {
        QString parent = urlPath.left(urlPath.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        html += "<div class='item'><a href='" + parent + "'>⬆ Back</a></div>";
    }

    // List folders first, then files
    QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name);

    for (const QFileInfo& entry : entries) {
        QString name    = entry.fileName();
        QString link    = urlPath.endsWith("/")
                              ? urlPath + name
                              : urlPath + "/" + name;
        QString icon    = entry.isDir() ? "📁" : "📄";
        QString size    = entry.isFile()
                              ? " (" + QString::number(entry.size() / 1024) + " KB)"
                              : "";
        html += "<div class='item'><a href='" + link + "'>"
                + icon + " " + name + size + "</a></div>";
    }

    html += "</body></html>";
    return html;
}
