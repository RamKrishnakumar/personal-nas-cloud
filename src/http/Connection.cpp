#include "http/Connection.h"

#include "http/Compression.h"
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
    case 504: return "Gateway Timeout";
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

Connection::Connection(QTcpSocket*             socket,
                       Router*                 router,
                       AdmissionControl*       admission,
                       const ConnectionLimits& limits,
                       QObject*                parent)
    : QObject(parent)
    , m_socket(socket)
    , m_router(router)
    , m_admission(admission)
    , m_limits(limits)
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
    m_idleTimer.start(m_limits.idleTimeoutMs);

    // Separate from the idle timer because the two watch different things: idle
    // means "connected but asking nothing", stall means "mid-transfer and not
    // draining". A stalled stream looks busy to the idle timer and would never be
    // reclaimed by it.
    m_stallTimer.setSingleShot(true);
    connect(&m_stallTimer, &QTimer::timeout, this, &Connection::onStallTimeout);

    m_admissionTimer.setSingleShot(false);
    connect(&m_admissionTimer, &QTimer::timeout, this, &Connection::retryAdmission);

    m_deferralTimer.setSingleShot(true);
    connect(&m_deferralTimer, &QTimer::timeout, this, [this]() {
        if (m_state != State::Deferred)
            return;
        m_state = State::ReadingHeaders;
        sendError(504);
    });

    m_admissionDeadline.setSingleShot(true);
    connect(&m_admissionDeadline, &QTimer::timeout, this, [this]() {
        m_admissionTimer.stop();
        if (m_state != State::AwaitingAdmission)
            return;

        // Honest refusal with a retry hint. The client is told to come back
        // rather than left waiting, and step 11's interface turns this into a
        // visible "waiting for a free transfer slot" rather than a dead page.
        m_state = State::ReadingHeaders;
        send(503, "text/plain; charset=utf-8", "Server busy — too many transfers",
             "Retry-After: 5\r\n");
    });

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

void Connection::deferResponse()
{
    m_responseStarted = true;
    m_state           = State::Deferred;

    m_idleTimer.stop();        // Waiting on our own work is not the client idling.
    m_deferralTimer.start(m_limits.deferralTimeoutMs);
}

void Connection::onIdleTimeout()
{
    // Only a client that is connected and asking nothing. A streaming, waiting or
    // deferred connection is mid-exchange, and closing it here would abort work
    // that is still progressing.
    if (m_state == State::ReadingHeaders)
        closeNow();
}

void Connection::onReadyRead()
{
    if (m_state == State::Closing)
        return;

    m_idleTimer.start(m_limits.idleTimeoutMs);
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

        if (++m_requestsServed >= m_limits.maxRequestsPerConnection)
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

    // ── Compression ───────────────────────────────────────────────────────
    // In-memory responses only. Compressing a streamed file would mean either
    // buffering the whole thing or giving up an exact Content-Length, and byte
    // ranges over a compressed stream are meaningless. Listings are the win here
    // anyway: repetitive markup, often several hundred kilobytes.
    QByteArray payload = body;
    QByteArray encodingHeaders;

    const bool negotiable = statusCode != 304 && isCompressibleContentType(contentType);

    if (negotiable
        && body.size() >= MinimumCompressibleBytes
        && acceptsGzip(m_request.header("accept-encoding"))) {

        const QByteArray compressed = gzipCompress(payload);

        // Falls back silently when compression fails or does not help. Sending a
        // larger body under a Content-Encoding header would be the worst outcome
        // available.
        if (!compressed.isEmpty() && compressed.size() < payload.size()) {
            payload         = compressed;
            encodingHeaders = "Content-Encoding: gzip\r\n";
        }
    }

    // Declared whenever the type is negotiable, not only when this particular
    // response was compressed. Otherwise a shared cache could hand a gzipped body
    // to a client that never asked for one.
    if (negotiable)
        encodingHeaders += "Vary: Accept-Encoding\r\n";

    // 304 and HEAD both carry headers only; Content-Length still describes what a
    // GET would have returned, which after compression is the compressed size.
    const bool omitBody = (statusCode == 304) || m_suppressBody;
    const qint64 declaredLength = (statusCode == 304) ? -1 : payload.size();
    m_socket->write(buildHeaders(statusCode, contentType, declaredLength,
                                 encodingHeaders + extraHeaders));
    if (!omitBody && !payload.isEmpty())
        m_socket->write(payload);

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

void Connection::sendFile(const QString& filesystemPath,
                          bool forceDownload,
                          QByteArrayView cacheControl)
{
    m_responseStarted = true;
    m_cacheControl    = cacheControl.toByteArray();

    const QFileInfo info(filesystemPath);
    const QByteArray etag = makeETag(info);

    // ── 304: the client already holds this exact file ─────────────────────
    // Answered before admission on purpose. It transfers no body, so making it
    // wait for a slot would penalise exactly the clients being most efficient.
    if (m_request.header("if-none-match") == QByteArrayView(etag)) {
        send(304, {}, {}, "ETag: " + etag + "\r\n"
                          "Cache-Control: " + m_cacheControl + "\r\n");
        return;
    }

    // HEAD sends no body, so it consumes no bandwidth and needs no slot.
    if (m_suppressBody || !m_admission) {
        beginFileResponse(filesystemPath, forceDownload);
        return;
    }

    AdmissionResult admitted = m_admission->request(
        peerAddress().toUtf8(), filesystemPath, info.size());

    if (admitted.outcome == Admission::AtCapacity) {
        // Queue rather than refuse outright. A visible wait reads as a working
        // system; an immediate error reads as a broken one.
        m_state                = State::AwaitingAdmission;
        m_pendingPath          = filesystemPath;
        m_pendingForceDownload = forceDownload;
        m_idleTimer.stop();              // Waiting is not idling.
        m_admissionTimer.start(m_limits.admissionRetryMs);
        m_admissionDeadline.start(m_limits.admissionWaitMs);
        return;
    }

    m_slot = std::move(admitted.slot);    // Empty after a bypass; held otherwise.
    beginFileResponse(filesystemPath, forceDownload);
}

void Connection::retryAdmission()
{
    if (m_state != State::AwaitingAdmission) {
        m_admissionTimer.stop();
        return;
    }

    AdmissionResult admitted = m_admission->request(
        peerAddress().toUtf8(), m_pendingPath, QFileInfo(m_pendingPath).size());

    if (admitted.outcome == Admission::AtCapacity)
        return;                           // Keep waiting until the deadline.

    m_admissionTimer.stop();
    m_admissionDeadline.stop();

    m_slot  = std::move(admitted.slot);
    m_state = State::ReadingHeaders;       // beginFileResponse sets Streaming.
    beginFileResponse(m_pendingPath, m_pendingForceDownload);
}

void Connection::beginFileResponse(const QString& filesystemPath, bool forceDownload)
{
    m_responseStarted = true;

    const QFileInfo info(filesystemPath);
    const qint64    fileSize = info.size();
    const QByteArray etag    = makeETag(info);

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
    extra += "Cache-Control: " + m_cacheControl + "\r\n";

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

    m_idleTimer.stop();                   // Streaming, not idle.
    m_stallTimer.start(m_limits.stallTimeoutMs);

    pumpFile();
}

void Connection::pumpFile()
{
    // Back-pressure: stop reading once enough is queued on the socket. Without
    // this, a fast disk and a slow client grow the write buffer without bound -
    // the failure mode that matters most on a phone over mobile data.
    while (m_fileRemaining > 0 && m_socket->bytesToWrite() < m_limits.writeHighWaterMark) {
        const qint64     wanted = qMin(m_limits.readChunkSize, m_fileRemaining);
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

void Connection::onStallTimeout()
{
    if (m_state != State::Streaming)
        return;

    // The client stopped draining. This is the case a FIN never arrives for — a
    // phone leaving coverage keeps the socket nominally connected indefinitely.
    // Abandoning it is what returns the stream slot; without this the pool would
    // be consumed permanently by clients that are already gone.
    qDebug() << "[HTTP] Stream stalled for" << peerAddress() << "— abandoning.";
    closeNow();
}

void Connection::onBytesWritten()
{
    if (m_state == State::Streaming) {
        // Progress was made, so restart the stall countdown.
        m_stallTimer.start(m_limits.stallTimeoutMs);
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

    m_stallTimer.stop();
    m_deferralTimer.stop();

    // Released as soon as the transfer ends, not when the connection closes. With
    // keep-alive a client may hold the socket open for another fifteen seconds,
    // and holding a slot through that idle time would waste a third of a small
    // pool.
    m_slot.release();

    if (!m_keepAlive) {
        m_state = State::Closing;
        if (m_socket->bytesToWrite() == 0)
            m_socket->disconnectFromHost();
        return;                                  // Flushed by onBytesWritten().
    }

    m_state = State::ReadingHeaders;
    m_idleTimer.start(m_limits.idleTimeoutMs);

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
    m_stallTimer.stop();
    m_deferralTimer.stop();
    m_admissionTimer.stop();
    m_admissionDeadline.stop();

    delete m_file;
    m_file          = nullptr;
    m_fileRemaining = 0;

    // The destructor would do this too; doing it here means the slot is free
    // before deleteLater() actually runs, which can be a full event-loop turn.
    m_slot.release();

    m_socket->abort();      // Immediate: no lingering half-written response.
    emit closed();
    deleteLater();
}

} // namespace http
