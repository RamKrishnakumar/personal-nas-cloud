#pragma once

#include <QObject>
#include <QSet>
#include <QSslConfiguration>

class QTcpSocket;

namespace http {

class Connection;
class Router;

/**
 * @class Worker
 * @brief Owns the sockets for one worker thread.
 *
 * Created on the GUI thread, then moved into its own QThread. Every slot runs in
 * that thread, so the sockets it creates, and the Connection objects that wrap
 * them, live and die there. All blocking disk reads for responses therefore
 * happen off the GUI thread — which is the entire point of step 5. In v0.5 a
 * single large download blocked the event loop that also drew the window.
 */
class Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(Router* router, QObject* parent = nullptr);
    ~Worker() override;

    /** Live connection count. Safe to read from any thread. */
    int connectionCount() const;

public slots:
    /**
     * @brief Adopts an accepted socket descriptor.
     *
     * Must be invoked through a queued connection so the body runs in the
     * worker's thread. Creating the socket here rather than on the accepting
     * thread is what keeps it thread-affine for its whole life: a connected
     * QSslSocket cannot safely be moved between threads mid-handshake.
     *
     * Takes responsibility for @p descriptor, including closing it if adoption
     * fails. Dropping it would leak a file descriptor per failed connection.
     */
    void adoptDescriptor(qintptr descriptor, bool useTls, QSslConfiguration tlsConfiguration);

    /** Closes every live connection. Call before stopping the thread. */
    void shutdown();

private:
    void registerConnection(QTcpSocket* socket);

    Router*             m_router = nullptr;
    QSet<Connection*>   m_connections;
    mutable QAtomicInt  m_connectionCount;
};

} // namespace http
