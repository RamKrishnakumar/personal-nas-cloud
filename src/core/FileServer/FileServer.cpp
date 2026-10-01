#include "core/FileServer/FileServer.h"

#include "handlers/FileRouter.h"
#include "http/HardwareTier.h"
#include "http/HttpServer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>

FileServer::FileServer(QObject* parent)
    : QObject(parent)
    , m_router(std::make_unique<handlers::FileRouter>(QStringLiteral("E:/")))
    , m_server(std::make_unique<http::HttpServer>(m_router.get()))
{
}

FileServer::~FileServer()
{
    // Explicit ordering: the server's worker threads must be joined before the
    // router they hold a pointer to is destroyed.
    m_server.reset();
    m_router.reset();
}

void FileServer::setNasRoot(const QString& path)
{
    QString root = path;
    while (root.size() > 1
           && (root.endsWith(QLatin1Char('/')) || root.endsWith(QLatin1Char('\\')))) {
        root.chop(1);
    }
    m_router->setNasRoot(root);
}

bool FileServer::loadTlsFiles(const QString& certificatePath, const QString& privateKeyPath)
{
    const auto resolve = [](const QString& path) {
        return QDir::isAbsolutePath(path)
                   ? path
                   : QCoreApplication::applicationDirPath() + QLatin1Char('/') + path;
    };

    const QString certFilePath = resolve(certificatePath);
    const QString keyFilePath  = resolve(privateKeyPath);

    QFile certFile(certFilePath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        m_tlsErrorString = QStringLiteral("Certificate file not found: %1").arg(certFilePath);
        return false;
    }
    const QSslCertificate certificate(&certFile, QSsl::Pem);
    certFile.close();

    if (certificate.isNull()) {
        m_tlsErrorString = QStringLiteral("Certificate is not valid PEM: %1").arg(certFilePath);
        return false;
    }

    QFile keyFile(keyFilePath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        m_tlsErrorString = QStringLiteral("Private key file not found: %1").arg(keyFilePath);
        return false;
    }
    const QSslKey privateKey(&keyFile, QSsl::Rsa, QSsl::Pem);
    keyFile.close();

    if (privateKey.isNull()) {
        m_tlsErrorString = QStringLiteral("Private key is not a valid RSA PEM key: %1")
                               .arg(keyFilePath);
        return false;
    }

    m_sslConfiguration = QSslConfiguration::defaultConfiguration();
    m_sslConfiguration.setLocalCertificate(certificate);
    m_sslConfiguration.setPrivateKey(privateKey);
    m_sslConfiguration.setProtocol(QSsl::TlsV1_2OrLater);
    m_sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);

    m_tlsErrorString.clear();
    qInfo() << "[TLS] ✅ Certificate loaded:" << certFilePath
            << "| expires" << certificate.expiryDate().toString(Qt::ISODate);
    return true;
}

bool FileServer::start(quint16 port)
{
    if (m_server->isListening())
        return true;

    // Detected here rather than in the constructor: the disk query needs the NAS
    // root, which MainWindow sets from config after construction. Whether the
    // served volume spins is the single most useful input on this hardware.
    http::HardwareProfile profile =
        http::detectHardwareProfile(m_router->nasRoot());

    if (m_streamSlotOverride > 0)
        profile = http::profileWithStreamSlots(profile, m_streamSlotOverride);

    m_server->setHardwareProfile(profile);
    qInfo().noquote() << "[Server] " << http::describeProfile(profile);

    if (m_useHttps) {
        if (m_sslConfiguration.isNull()) {
            qWarning() << "[Server] ❌ HTTPS requested but no certificate loaded.";
            return false;
        }
        m_server->setTlsConfiguration(m_sslConfiguration);
    } else {
        m_server->clearTlsConfiguration();
    }

    m_port = port;
    if (!m_server->start(port))
        return false;

    emit serverStarted(localIP(), m_port);
    return true;
}

void FileServer::stop()
{
    if (!m_server->isListening())
        return;

    m_server->stop();
    emit serverStopped();
}

bool FileServer::isRunning() const
{
    return m_server->isListening();
}

int FileServer::connectionCount() const
{
    return m_server->connectionCount();
}

int FileServer::activeStreams() const
{
    return m_server->activeStreams();
}

QString FileServer::localIP() const
{
    // Prefer a private LAN address. Picking the first non-loopback entry, as v0.5
    // did, can land on a VPN or virtual adapter that other devices cannot reach.
    QString fallback;

    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;

            if (address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)
                || address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)
                || address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)) {
                return address.toString();
            }

            if (fallback.isEmpty())
                fallback = address.toString();
        }
    }

    return fallback.isEmpty() ? QStringLiteral("127.0.0.1") : fallback;
}
