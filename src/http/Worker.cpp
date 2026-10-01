#include "http/Worker.h"

#include "http/Connection.h"

#include <QDebug>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>

#ifdef Q_OS_WIN
// See the note in HttpServer.cpp: windows.h defines min and max as macros unless
// NOMINMAX is set first, and the resulting errors point at unrelated lines.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#else
#  include <unistd.h>
#endif

namespace {

/** Seconds a TLS handshake may take before the connection is dropped. */
constexpr int HandshakeTimeoutMs = 8'000;

/**
 * Closes a descriptor Qt never took ownership of.
 *
 * Needed when setSocketDescriptor() fails: the QAbstractSocket did not adopt it,
 * so deleting the socket leaves the descriptor open. A few hundred of those
 * exhausts the process's handle table.
 */
void closeRawDescriptor(qintptr descriptor)
{
#ifdef Q_OS_WIN
    ::closesocket(static_cast<SOCKET>(descriptor));
#else
    ::close(static_cast<int>(descriptor));
#endif
}

} // namespace

namespace http {

Worker::Worker(Router* router, QObject* parent)
    : QObject(parent)
    , m_router(router)
{
}

Worker::~Worker() = default;

int Worker::connectionCount() const
{
    return m_connectionCount.loadRelaxed();
}

void Worker::adoptDescriptor(qintptr descriptor,
                             bool useTls,
                             QSslConfiguration tlsConfiguration)
{
    if (!useTls) {
        auto* socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor, QAbstractSocket::ConnectedState)) {
            qWarning() << "[HTTP] Could not adopt descriptor:" << socket->errorString();
            socket->deleteLater();
            closeRawDescriptor(descriptor);
            return;
        }
        registerConnection(socket);
        return;
    }

    // ── TLS ───────────────────────────────────────────────────────────────
    auto* socket = new QSslSocket(this);
    if (!socket->setSocketDescriptor(descriptor, QAbstractSocket::ConnectedState)) {
        qWarning() << "[TLS] Could not adopt descriptor:" << socket->errorString();
        socket->deleteLater();
        closeRawDescriptor(descriptor);
        return;
    }

    socket->setSslConfiguration(tlsConfiguration);

    // Peer verification is off by design: this is a server presenting its own
    // certificate, not a client checking one. Any error reported here concerns
    // the absent client certificate, so logging and continuing is correct.
    connect(socket, &QSslSocket::sslErrors, socket,
            [socket](const QList<QSslError>& errors) {
        for (const QSslError& error : errors)
            qDebug() << "[TLS] ⚠️" << error.errorString();
        socket->ignoreSslErrors();
    });

    // The Connection is created only once the channel is encrypted, so no
    // handler can ever see plaintext from an unfinished handshake.
    connect(socket, &QSslSocket::encrypted, this, [this, socket]() {
        registerConnection(socket);
    });

    // A peer that opens a socket and never completes the handshake would
    // otherwise hold one of the bounded connection slots indefinitely.
    auto* handshakeTimer = new QTimer(socket);
    handshakeTimer->setSingleShot(true);
    connect(handshakeTimer, &QTimer::timeout, socket, [socket]() {
        if (!socket->isEncrypted()) {
            qDebug() << "[TLS] Handshake timed out for"
                     << socket->peerAddress().toString();
            socket->abort();
        }
    });
    connect(socket, &QSslSocket::encrypted, handshakeTimer, &QTimer::stop);
    handshakeTimer->start(HandshakeTimeoutMs);

    // If the handshake fails outright, nothing will ever create a Connection,
    // so the socket has to clean itself up.
    connect(socket, &QAbstractSocket::disconnected, socket, [socket]() {
        if (!socket->isEncrypted())
            socket->deleteLater();
    });

    socket->startServerEncryption();
}

void Worker::registerConnection(QTcpSocket* socket)
{
    auto* connection = new Connection(socket, m_router, this);

    m_connections.insert(connection);
    m_connectionCount.storeRelaxed(m_connections.size());

    connect(connection, &Connection::closed, this, [this, connection]() {
        m_connections.remove(connection);
        m_connectionCount.storeRelaxed(m_connections.size());
    });
}

void Worker::shutdown()
{
    // Copied first: closing a Connection emits closed(), which mutates the set.
    const QSet<Connection*> live = m_connections;
    for (Connection* connection : live)
        delete connection;

    m_connections.clear();
    m_connectionCount.storeRelaxed(0);
}

} // namespace http
