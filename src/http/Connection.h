#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include "http/AdmissionControl.h"
#include "http/RequestParser.h"

class QFile;
class QTcpSocket;

namespace http {

class Router;

/**
 * @brief Per-connection tunables, sized from the hardware tier.
 *
 * Runtime values rather than compile-time constants because the right numbers
 * depend on the machine: 1 MB reads suit a bounded number of streams on a
 * spinning disk, and would be the wrong trade without the stream cap.
 */
struct ConnectionLimits
{
    /** Bytes read from disk per pump iteration. From HardwareProfile. */
    qint64 readChunkSize = 1024 * 1024;

    /** Stop reading more file data once this much is queued on the socket. */
    qint64 writeHighWaterMark = 512 * 1024;

    /** Idle time between requests before closing. */
    int idleTimeoutMs = 15'000;

    /**
     * Longest a stream may make no progress before being abandoned.
     *
     * This is what reclaims a slot held by a client that vanished without
     * closing — a phone leaving mobile coverage never sends a FIN, so the socket
     * stays nominally connected forever. Without it, a few such transfers would
     * exhaust the pool and the server would appear to hang.
     */
    int stallTimeoutMs = 30'000;

    /** Requests served on one connection before it is closed anyway. */
    int maxRequestsPerConnection = 200;

    /** How long to keep retrying for a stream slot before answering 503. */
    int admissionWaitMs = 10'000;

    /** Interval between admission retries while waiting. */
    int admissionRetryMs = 250;
};

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
    /**
     * @param socket     Already connected, and already encrypted when TLS is in
     *                   use. Connection takes ownership.
     * @param admission  Shared across workers; may be null, in which case every
     *                   transfer proceeds unmetered.
     */
    Connection(QTcpSocket*             socket,
               Router*                 router,
               AdmissionControl*       admission,
               const ConnectionLimits& limits,
               QObject*                parent = nullptr);
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
    enum class State { ReadingHeaders, AwaitingAdmission, Streaming, Closing };

    void onReadyRead();
    void onBytesWritten();
    void onIdleTimeout();
    void onStallTimeout();

    /** Parses and dispatches as many whole requests as the buffer holds. */
    void processBuffer();

    /** Asks for a slot and either begins streaming or starts waiting. */
    void beginFileResponse(const QString& filesystemPath, bool forceDownload);

    /** Retries admission while in AwaitingAdmission. */
    void retryAdmission();

    void pumpFile();
    void finishResponse();
    void closeNow();

    /** Header block shared by every response. */
    QByteArray buildHeaders(int statusCode,
                            QByteArrayView contentType,
                            qint64 contentLength,
                            const QByteArray& extraHeaders) const;

    QTcpSocket*       m_socket    = nullptr;
    Router*           m_router    = nullptr;
    AdmissionControl* m_admission = nullptr;
    ConnectionLimits  m_limits;

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

    /** Held for the duration of a metered transfer; released by finish or abort. */
    StreamSlot m_slot;

    // Pending request details while in AwaitingAdmission.
    QString m_pendingPath;
    bool    m_pendingForceDownload = false;
    QTimer  m_admissionTimer;
    QTimer  m_admissionDeadline;

    QTimer m_idleTimer;

    /** Reset on every write; fires when a stream makes no progress. */
    QTimer m_stallTimer;
};

} // namespace http
