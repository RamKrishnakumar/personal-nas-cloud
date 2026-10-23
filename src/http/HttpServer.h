#pragma once

#include <QList>
#include <QSslConfiguration>
#include <QTcpServer>

#include <memory>

#include "http/Connection.h"
#include "http/HardwareTier.h"

class QThread;

namespace http {

class AdmissionControl;
class Router;
class Worker;

/**
 * @class HttpServer
 * @brief Accepts connections on the GUI thread and hands them to a worker pool.
 *
 * ### Why this subclasses QTcpServer
 *
 * nextPendingConnection() returns a QTcpSocket created on the accepting thread,
 * which is the wrong thread and the wrong class for TLS. v0.5 worked around that
 * by taking the descriptor out of that socket and calling
 * setSocketDescriptor(-1) on it so its destructor would not close the descriptor
 * it no longer owned.
 *
 * Overriding incomingConnection(qintptr) removes the need for any of it: Qt never
 * constructs a socket, and the raw descriptor goes straight to a worker, which
 * creates the right socket type in its own thread.
 */
class HttpServer : public QTcpServer
{
    Q_OBJECT

public:
    /** Hard ceiling on live connections, independent of the stream cap in step 8. */
    static constexpr int MaxConnections = 32;

    /**
     * @param router Shared by every worker thread, so it must be thread-safe.
     *               Must outlive this server.
     */
    explicit HttpServer(Router* router, QObject* parent = nullptr);
    ~HttpServer() override;

    /**
     * @brief Replaces the router.
     *
     * Exists so an authentication gate can be wrapped around the original router
     * after construction, which is when the owner's configuration is known. Workers
     * capture the pointer when they are created in start(), so this must be called
     * before then — calling it on a listening server would leave existing workers
     * pointing at the old router and is refused.
     *
     * @return false when the server is already listening.
     */
    bool setRouter(Router* router);

    /**
     * @brief Worker thread count.
     *
     * Defaults to idealThreadCount clamped to [2, 4]. Serving files is IO-bound,
     * so more threads than that add context switching without throughput — and on
     * the reference 2-core laptop they would compete with the UI. Step 8 feeds
     * this from hardware tier detection.
     *
     * Must be set before listen().
     */
    void setWorkerCount(int count);
    int  workerCount() const;

    /**
     * @brief Applies a detected or user-overridden hardware profile.
     *
     * Sets worker count, stream slots and read chunk size together, because they
     * are one trade-off rather than three: larger reads are only the right choice
     * because the number of concurrent streams is bounded.
     *
     * Must be called before listen().
     */
    void setHardwareProfile(const HardwareProfile& profile);
    const HardwareProfile& hardwareProfile() const { return m_profile; }

    /** Concurrent metered transfers across all workers. */
    int activeStreams() const;

    /**
     * @brief Overrides per-connection timeouts and buffer sizes.
     *
     * setHardwareProfile() already sets the chunk size, so this is for the
     * remaining knobs — timeouts in particular, which tests need to shorten.
     * Must be called before listen().
     */
    void setConnectionLimits(const ConnectionLimits& limits);
    const ConnectionLimits& connectionLimits() const { return m_limits; }

    /** The shared admission controller, or null before a profile is applied. */
    AdmissionControl* admissionControl() const { return m_admission.get(); }

    /**
     * @brief Enables TLS, or replaces the certificate already in use.
     *
     * Takes effect for connections accepted after this call. Ones already
     * established keep the certificate they completed their handshake with, so
     * replacing an expiring certificate does not interrupt a download in progress.
     *
     * Safe while listening, unlike the other setters here, because the configuration
     * is read per connection on this object's own thread rather than captured by the
     * workers when they start. Must be called from that thread.
     */
    void setTlsConfiguration(const QSslConfiguration& configuration);
    void clearTlsConfiguration();
    bool usesTls() const { return m_useTls; }

    /** Live connections across every worker. */
    int connectionCount() const;

    /** Starts worker threads and listens. */
    bool start(quint16 port, const QHostAddress& address = QHostAddress::AnyIPv4);

    /** Stops listening, closes live connections, and joins the worker threads. */
    void stop();

protected:
    /**
     * @brief Called by Qt on the accepting thread for each new connection.
     *
     * Does no I/O and creates no socket: it picks a worker and posts the
     * descriptor across. Anything slow here would stall accepting for everyone.
     */
    void incomingConnection(qintptr socketDescriptor) override;

private:
    void startWorkers();
    void stopWorkers();

    /** Least-loaded worker, so one slow client cannot pile work on one thread. */
    Worker* chooseWorker() const;

    Router* m_router = nullptr;

    std::unique_ptr<AdmissionControl> m_admission;
    HardwareProfile                   m_profile;
    ConnectionLimits                  m_limits;

    QList<QThread*> m_threads;
    QList<Worker*>  m_workers;

    int  m_workerCount = 0;
    bool m_useTls      = false;
    QSslConfiguration m_tlsConfiguration;
};

} // namespace http
