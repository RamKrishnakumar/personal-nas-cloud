#pragma once

#include <QList>
#include <QSslConfiguration>
#include <QTcpServer>

class QThread;

namespace http {

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

    /** Enables TLS for subsequent connections. Must be set before listen(). */
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

    QList<QThread*> m_threads;
    QList<Worker*>  m_workers;

    int  m_workerCount = 0;
    bool m_useTls      = false;
    QSslConfiguration m_tlsConfiguration;
};

} // namespace http
