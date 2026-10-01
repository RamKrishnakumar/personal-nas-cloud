#include "http/Connection.h"

#include "http/MimeType.h"
#include "http/RangeParser.h"
#include "http/Router.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>

namespace {

QByteArrayView reasonPhrase(int statusCode)
{
    switch (statusCode) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

/** Maps a parse failure onto the status code it must produce. */
int statusForParseFailure(http::ParseStatus status)
{
    switch (status) {
    case http::ParseStatus::UriTooLong:          return 414;
    case http::ParseStatus::HeadersTooLarge:     return 431;
    case http::ParseStatus::NotImplemented:      return 501;
    case http::ParseStatus::VersionNotSupported: return 505;
    default:                                     return 400;
    }
}

/** Weak-ish validator over path, size and modification time. */
QByteArray makeETag(const QFileInfo& info)
{
    const QByteArray material = info.absoluteFilePath().toUtf8()
                                + QByteArray::number(info.size())
                                + QByteArray::number(info.lastModified().toMSecsSinceEpoch());

    return '"' + QCryptographicHash::hash(material, QCryptographicHash::Md5)
                     .toHex().left(16) + '"';
}

} // namespace

namespace http {

Connection::Connection(QTcpSocket* socket, Router* router, QObject* parent)
    : QObject(parent)
    , m_socket(socket)
    , m_router(router)
{
    m_socket->setParent(this);

    // Nagle off: responses are written as whole header blocks followed by large
    // chunks, so coalescing only adds latency on a high-RTT link.
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(m_socket, &QIODevice::readyRead,    this, &Connection::onReadyRead);
    connect(m_socket, &QIODevice::bytesWritten, this, &Connection::onBytesWritten);

    connect(m_socket, &QAbstractSocket::disconnected, this, [this]() {
        emit closed();
        deleteLater();
    });

    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, &Connection::onIdleTimeout);
    m_idleTimer.start(IdleTimeoutMs);

    // TLS buffers the first request during the handshake, so bytes can already be
    // waiting by the time we get here. Without this, such a request is only
    // noticed if the client happens to send more.
    if (m_socket->bytesAvailable() > 0)
        onReadyRead();
}

Connection::~Connection()
{
    delete m_file;
}

QString Connection::peerAddress() const
{
    return m_socket->peerAddress().toString();
}

void Connection::onIdleTimeout()
{
    // Nothing is in flight, so this is a client holding a socket open for no
    // reason. Closing reclaims one of the bounded connection slots.
    if (m_state != State::Streaming)
        closeNow();
}

void Connection::onReadyRead()
{
    if (m_state == State::Closing)
        return;

    m_idleTimer.start(IdleTimeoutMs);
    m_inBuffer += m_socket->readAll();

    if (m_state == State::ReadingHeaders)
        processBuffer();
}

void Connection::processBuffer()
{
    // Loops because one read can contain several pipelined requests. Stops as
    // soon as a response begins streaming - the next request must wait until
    // this one's bytes are fully written, or the two would interleave.
    while (m_state == State::ReadingHeaders && !m_inBuffer.isEmpty()) {
        const ParseResult result = parseRequest(m_inBuffer);

        if (result.status == ParseStatus::Incomplete)
            return;                                  // Wait for more bytes.

        if (result.status != ParseStatus::Complete) {
            m_keepAlive = false;                     // Framing is untrustworthy now.
            sendError(statusForParseFailure(result.status));
            return;
        }

        m_inBuffer.remove(0, result.consumed);
        m_request   = result.request;
        m_keepAlive = m_request.keepAlive;

        if (++m_requestsServed >= MaxRequestsPerConnection)
            m_keepAlive = false;

        // No endpoint accepts a body yet. Draining one would be the alternative,
        // but refusing and closing cannot desynchronise the stream.
        if (m_request.contentLength > 0) {
            m_keepAlive = false;
            sendError(405);
            return;
        }

        m_responseStarted = false;


        m_suppressBody   = (m_request.method == "HEAD");
        m_router->handle(*this, m_request);

        // A Router that returns without responding would otherwise leave the
        // client waiting for the idle timeout with no explanation.
        if (!m_responseStarted) {
            m_keepAlive = false;
            sendError(500);
            return;
        }
    }
}

QByteArray Connection::buildHeaders(int statusCode,
                                    QByteArrayView contentType,
                                    qint64 contentLength,
                                    const QByteArray& extraHeaders) const
{
    QByteArray head = "HTTP/1.1 " + QByteArray::number(statusCode) + ' '
                      + reasonPhrase(statusCode).toByteArray() + "\r\n";

    if (!contentType.isEmpty())
        head += "Content-Type: " + contentType.toByteArray() + "\r\n";

    // Always exact. Keep-alive without accurate framing corrupts whatever
    // response follows on the same connection.
    if (contentLength >= 0)
        head += "Content-Length: " + QByteArray::number(contentLength) + "\r\n";

    head += m_keepAlive ? "Connection: keep-alive\r\n"
                        : "Connection: close\r\n";

    head += extraHeaders;
    head += "\r\n";
    return head;
}

void Connection::send(int statusCode,
                      QByteArrayView contentType,
                      const QByteArray& body,
                      const QByteArray& extraHeaders)
{
    m_responseStarted = true;

    // 304 and HEAD both carry headers only; Content-Length still describes what
    // a GET would have returned.
    const bool omitBody = (statusCode == 304) || m_suppressBody;
    const qint64 declaredLength = (statusCode == 304) ? -1 : body.size();
    m_socket->write(buildHeaders(statusCode, contentType,
                                 declaredLength, extraHeaders));
    if (!omitBody && !body.isEmpty())
        m_socket->write(body);

    finishResponse();
}

void Connection::sendHtml(const QString& html)
{
    // Listings reflect live directory contents, so they must never be cached.
    send(200, "text/html; charset=utf-8", html.toUtf8(),
         "Cache-Control: no-store\r\n");
}

void Connection::sendError(int statusCode)
{
    const QByteArray reason = reasonPhrase(statusCode).toByteArray();
    const QByteArray body = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                            "<title>" + QByteArray::number(statusCode) + "</title></head>"
                            "<body style='font-family:sans-serif;background:#0a0a14;"
                            "color:#ccc;padding:2rem'><h1>"
                            + QByteArray::number(statusCode) + " " + reason
                            + "</h1></body></html>";

    send(statusCode, "text/html; charset=utf-8", body, "Cache-Control: no-store\r\n");
}

void Connection::sendFile(const QString& filesystemPath, bool forceDownload)
{
    m_responseStarted = true;

    const QFileInfo info(filesystemPath);
    const qint64    fileSize = info.size();
    const QByteArray etag    = makeETag(info);

    // ── 304: the client already holds this exact file ─────────────────────
    if (m_request.header("if-none-match") == QByteArrayView(etag)) {
        send(304, {}, {}, "ETag: " + etag + "\r\n"
                          "Cache-Control: private, max-age=3600\r\n");
        return;
    }

    auto* file = new QFile(filesystemPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        m_responseStarted = false;
        sendError(403);
        return;
    }

    const ByteRange range = parseRange(m_request.header("range"), fileSize);

    // ── 416: a well-formed range with no valid answer ─────────────────────
    if (range.present && !range.satisfiable) {
        delete file;
        m_responseStarted = false;
        m_keepAlive = m_request.keepAlive;
        send(416, "text/plain; charset=utf-8", "Range Not Satisfiable",
             "Content-Range: bytes */" + QByteArray::number(fileSize) + "\r\n");
        return;
    }

    const bool    partial = range.present && range.satisfiable;
    const qint64  start   = partial ? range.start : 0;
    const qint64  length  = partial ? range.length() : fileSize;

    if (start > 0 && !file->seek(start)) {
        delete file;
        m_responseStarted = false;
        sendError(500);
        return;
    }

    QByteArray extra;
    extra += "Accept-Ranges: bytes\r\n";
    extra += "ETag: " + etag + "\r\n";
    extra += "Cache-Control: private, max-age=3600\r\n";

    if (forceDownload) {
        // Only the quote and backslash need escaping inside a quoted-string.
        QByteArray name = info.fileName().toUtf8();
        name.replace('\\', "\\\\").replace('"', "\\\"");
        extra += "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
    }

    if (partial) {
        extra += "Content-Range: bytes " + QByteArray::number(range.start) + '-'
                 + QByteArray::number(range.end) + '/'
                 + QByteArray::number(fileSize) + "\r\n";
    }

    m_socket->write(buildHeaders(partial ? 206 : 200,
                                 mimeTypeForPath(filesystemPath),
                                 length, extra));

    // HEAD: the headers above already describe the body, so there is nothing
    // further to send. Streaming it and discarding would read the whole file for
    // a client that asked not to receive it.
    if (m_suppressBody) {
        delete file;
        finishResponse();
        return;
    }

    m_file          = file;
    m_fileRemaining = length;
    m_state         = State::Streaming;

    pumpFile();
}

void Connection::pumpFile()
{
    // Back-pressure: stop reading once enough is queued on the socket. Without
    // this, a fast disk and a slow client grow the write buffer without bound -
    // the failure mode that matters most on a phone over mobile data.
    while (m_fileRemaining > 0 && m_socket->bytesToWrite() < WriteHighWaterMark) {
        const qint64     wanted = qMin(ReadChunkSize, m_fileRemaining);
        const QByteArray chunk  = m_file->read(wanted);

        if (chunk.isEmpty()) {
            // Truncated or unreadable mid-stream. The declared Content-Length can
            // no longer be met, so the only honest signal left is to drop the
            // connection; a short body would look like a complete file.
            closeNow();
            return;
        }

        if (m_socket->write(chunk) < 0) {
            closeNow();
            return;
        }

        m_fileRemaining -= chunk.size();
    }

    if (m_fileRemaining == 0 && m_socket->bytesToWrite() == 0)
        finishResponse();
}

void Connection::onBytesWritten()
{
    if (m_state == State::Streaming) {
        pumpFile();
        return;
    }

    // An in-memory response finishes once its bytes have actually left.
    if (m_state == State::Closing && m_socket->bytesToWrite() == 0)
        m_socket->disconnectFromHost();
}

void Connection::finishResponse()
{
    delete m_file;
    m_file          = nullptr;
    m_fileRemaining = 0;

    if (!m_keepAlive) {
        m_state = State::Closing;
        if (m_socket->bytesToWrite() == 0)
            m_socket->disconnectFromHost();
        return;                                  // Flushed by onBytesWritten().
    }

    m_state = State::ReadingHeaders;
    m_idleTimer.start(IdleTimeoutMs);

    // A pipelined request may already be sitting in the buffer. Nothing will
    // wake us for bytes that arrived before the previous response was written,
    // so process them now rather than waiting for a readyRead that never comes.
    if (!m_inBuffer.isEmpty())
        processBuffer();
}

void Connection::closeNow()
{
    m_state = State::Closing;
    m_idleTimer.stop();

    delete m_file;
    m_file          = nullptr;
    m_fileRemaining = 0;

    m_socket->abort();      // Immediate: no lingering half-written response.
    emit closed();
    deleteLater();
}

} // namespace http
