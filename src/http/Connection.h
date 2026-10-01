#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include "http/RequestParser.h"

class QFile;
class QTcpSocket;

namespace http {

class Router;

/**
 * @class Connection
 * @brief One client socket: framing, keep-alive, and response streaming.
 *
 * Lives entirely in the worker thread that accepted the descriptor. Nothing here
 * is thread-safe, and nothing needs to be — the socket, the buffer and the file
 * handle are only ever touched from that one thread.
 *
 * ### Why a state machine
 *
 * v0.5 called readAll() on every readyRead and parsed whatever had arrived. That
 * assumes one read contains exactly one whole request, which is false on a slow
 * or TLS-wrapped link. It also forced Connection: close on every response,
 * because without exact framing a reused connection corrupts the next response.
 *
 * Keeping explicit state lets a request arrive over several reads, lets several
 * pipelined requests arrive in one read, and makes keep-alive safe.
 *
 * ### Lifetime
 *
 * Self-deleting: emits closed() and calls deleteLater() once the socket is gone.
 * The owning Worker drops its reference on that signal.
 */
class Connection : public QObject
{
    Q_OBJECT

public:
    /** Stop reading more file data once this much is still queued on the socket. */
    static constexpr qint64 WriteHighWaterMark = 512 * 1024;

    /** Bytes read from disk per pump iteration. Raised with the stream cap in step 8. */
    static constexpr qint64 ReadChunkSize = 256 * 1024;

    /** Idle time between requests before the connection is closed. */
    static constexpr int IdleTimeoutMs = 15'000;

    /**
     * Requests served on one connection before it is closed anyway.
     * Bounds the effect of any per-request resource we fail to release.
     */
    static constexpr int MaxRequestsPerConnection = 200;

    /**
     * @param socket Already connected, and already encrypted when TLS is in use.
     *               Connection takes ownership.
     */
    Connection(QTcpSocket* socket, Router* router, QObject* parent = nullptr);
    ~Connection() override;

    /** The request currently being handled. Valid only inside Router::handle(). */
    const Request& request() const { return m_request; }

    QString peerAddress() const;

    // ── Response API, called by a Router ──────────────────────────────────

    /** Sends a complete in-memory response. */
    void send(int statusCode,
              QByteArrayView contentType,
              const QByteArray& body,
              const QByteArray& extraHeaders = {});

    /** Sends an HTML page with no-store caching, for directory listings. */
    void sendHtml(const QString& html);

    /** Sends a minimal HTML error page for @p statusCode. */
    void sendError(int statusCode);

    /**
     * @brief Streams a file, honouring the current request's Range header.
     *
     * Handles 200, 206, 304 and 416 itself, including exact Content-Length, and
     * stops after the requested range rather than at end of file.
     *
     * @param forceDownload Sends Content-Disposition: attachment when true.
     */
    void sendFile(const QString& filesystemPath, bool forceDownload);

signals:
    void closed();

private:
    enum class State { ReadingHeaders, Streaming, Closing };

    void onReadyRead();
    void onBytesWritten();
    void onIdleTimeout();

    /** Parses and dispatches as many whole requests as the buffer holds. */
    void processBuffer();

    void pumpFile();
    void finishResponse();
    void closeNow();

    /** Header block shared by every response. */
    QByteArray buildHeaders(int statusCode,
                            QByteArrayView contentType,
                            qint64 contentLength,
                            const QByteArray& extraHeaders) const;

    QTcpSocket* m_socket = nullptr;
    Router*     m_router = nullptr;

    QByteArray m_inBuffer;
    State      m_state = State::ReadingHeaders;
    Request    m_request;

    bool m_keepAlive       = true;
    bool m_responseStarted = false;   ///< Guards against a Router sending twice.
    int  m_requestsServed  = 0;

    /**
     * Set for HEAD. Headers must be byte-identical to the GET response,
     * including Content-Length, but no body may follow.
     */
    bool m_suppressBody = false;

    QFile* m_file          = nullptr;
    qint64 m_fileRemaining = 0;

    QTimer m_idleTimer;
};

} // namespace http
