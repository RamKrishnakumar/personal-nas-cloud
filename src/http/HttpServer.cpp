#include "http/HttpServer.h"

#include "http/Worker.h"

#include <QDebug>
#include <QThread>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#else
#  include <unistd.h>
#endif

namespace {

/** Closes a descriptor Qt has not wrapped in a socket yet. */
void closeRawDescriptor(qintptr descriptor)
{
#ifdef Q_OS_WIN
    ::closesocket(static_cast<SOCKET>(descriptor));
#else
    ::close(static_cast<int>(descriptor));
#endif
}

int defaultWorkerCount()
{
    return std::clamp(QThread::idealThreadCount(), 2, 4);
}

} // namespace

namespace http {

HttpServer::HttpServer(Router* router, QObject* parent)
    : QTcpServer(parent)
    , m_router(router)
    , m_workerCount(defaultWorkerCount())
{
    // Qt's own backlog guard would hold descriptors in an internal queue rather
    // than refusing them. The explicit check in incomingConnection() is what
    // enforces MaxConnections, so this is set high enough to stay out of the way.
    setMaxPendingConnections(MaxConnections * 2);
}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::setWorkerCount(int count)
{
    Q_ASSERT(!isListening());
    m_workerCount = std::clamp(count, 1, 16);
}

int HttpServer::workerCount() const
{
    return m_workerCount;
}

void HttpServer::setTlsConfiguration(const QSslConfiguration& configuration)
{
    Q_ASSERT(!isListening());
    m_tlsConfiguration = configuration;
    m_useTls = true;
}

void HttpServer::clearTlsConfiguration()
{
    m_tlsConfiguration = QSslConfiguration();
    m_useTls = false;
}

int HttpServer::connectionCount() const
{
    int total = 0;
    for (const Worker* worker : m_workers)
        total += worker->connectionCount();
    return total;
}

bool HttpServer::start(quint16 port, const QHostAddress& address)
{
    if (isListening())
        return true;

    startWorkers();

    if (!listen(address, port)) {
        qWarning() << "[HTTP] Listen failed on port" << port << ':' << errorString();
        stopWorkers();
        return false;
    }

    qInfo() << "[HTTP] Listening on" << address.toString() << "port" << port
            << "| workers:" << m_workers.size()
            << "| TLS:" << (m_useTls ? "on" : "off");
    return true;
}

void HttpServer::stop()
{
    if (isListening())
        close();

    stopWorkers();
}

void HttpServer::startWorkers()
{
    if (!m_workers.isEmpty())
        return;

    for (int index = 0; index < m_workerCount; ++index) {
        auto* thread = new QThread(this);
        thread->setObjectName(QStringLiteral("nestdrive-worker-%1").arg(index));

        // No parent: a QObject cannot be moved to another thread while it has a
        // parent living in this one. The thread owns it instead, via deleteLater
        // below.
        auto* worker = new Worker(m_router);
        worker->moveToThread(thread);

        connect(thread, &QThread::finished, worker, &QObject::deleteLater);

        thread->start();

        m_threads.append(thread);
        m_workers.append(worker);
    }
}

void HttpServer::stopWorkers()
{
    // Close live connections inside each worker's own thread. A blocking
    // invocation is needed so every socket is gone before the thread is asked to
    // quit; otherwise a Connection could still be mid-write as its event loop
    // stops, and its destructor would run on the wrong thread.
    for (Worker* worker : m_workers)
        QMetaObject::invokeMethod(worker, &Worker::shutdown, Qt::BlockingQueuedConnection);

    for (QThread* thread : m_threads) {
        thread->quit();
        if (!thread->wait(5'000)) {
            qWarning() << "[HTTP] Worker thread did not stop in time; terminating.";
            thread->terminate();
            thread->wait();
        }
    }

    qDeleteAll(m_threads);
    m_threads.clear();
    m_workers.clear();        // Each worker is deleted by its thread's finished signal.
}

Worker* HttpServer::chooseWorker() const
{
    Worker* best  = nullptr;
    int     fewest = std::numeric_limits<int>::max();

    for (Worker* worker : m_workers) {
        const int count = worker->connectionCount();
        if (count < fewest) {
            fewest = count;
            best   = worker;
        }
    }
    return best;
}

void HttpServer::incomingConnection(qintptr socketDescriptor)
{
    Worker* worker = chooseWorker();
    if (!worker) {
        // Listening without workers should be impossible, but dropping the
        // descriptor silently would leak it.
        qWarning() << "[HTTP] No worker available; dropping connection.";
        closeRawDescriptor(socketDescriptor);
        return;
    }

    if (connectionCount() >= MaxConnections) {
        // Refuse at the accept stage. Replying 503 would mean building a
        // Connection, which is the very resource being protected - and a port
        // scanner opening thousands of sockets is the case this guards against.
        qWarning() << "[HTTP] Connection limit" << MaxConnections
                   << "reached; refusing connection.";
        closeRawDescriptor(socketDescriptor);
        return;
    }

    // Queued by necessity: this runs on the accepting thread, and everything
    // about the socket must happen in the worker's thread instead.
    //
    // The functor overload rather than the "adoptDescriptor" string form: a
    // queued call by name requires every argument type to be a registered
    // metatype, and neither qintptr nor QSslConfiguration is guaranteed to be
    // one. That failure surfaces only as a runtime warning on stderr, and the
    // call silently does nothing. A lambda captures by value and is checked at
    // compile time.
    const bool useTls = m_useTls;
    const QSslConfiguration tlsConfiguration = m_tlsConfiguration;

    QMetaObject::invokeMethod(worker, [worker, socketDescriptor, useTls, tlsConfiguration]() {
        worker->adoptDescriptor(socketDescriptor, useTls, tlsConfiguration);
    }, Qt::QueuedConnection);
}

} // namespace http
