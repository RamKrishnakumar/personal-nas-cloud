#include "core/FileServer/FileServer.h"

#include "auth/LoginRateLimiter.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/HardwareTier.h"
#include "http/HttpServer.h"
#include "media/ThumbnailCache.h"
#include "media/ThumbnailScanner.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>

FileServer::FileServer(QObject* parent)
    : QObject(parent)
    , m_router(std::make_unique<handlers::FileRouter>(QStringLiteral("E:/")))
    , m_server(std::make_unique<http::HttpServer>(m_router.get()))
{
}

FileServer::~FileServer()
{
    // Explicit ordering: the worker threads must be joined before the router they
    // hold a pointer to, and the router before the things it points at.
    m_server.reset();
    m_router.reset();
    m_scanner.reset();      // Joins its thread before the cache it uses goes away.
    m_thumbnails.reset();

    m_sessionSweeper.reset();
    m_authentication.reset();   // Joins its hashing thread.
    m_limiter.reset();
    m_sessions.reset();
    m_users.reset();
}

void FileServer::setThumbnailStore(db::Database* database, const QString& cacheDirectory)
{
    // Generator threads come from the same tier detection as everything else, but
    // the profile is only established in start(). One thread is the conservative
    // choice and matches the Light tier, which is the target hardware.
    const http::HardwareProfile detected = http::detectHardwareProfile();

    m_thumbnails = std::make_unique<media::ThumbnailCache>(
        database, cacheDirectory, detected.thumbnailThreads);

    m_router->setThumbnailCache(m_thumbnails.get());

    m_scanner = std::make_unique<media::ThumbnailScanner>(m_thumbnails.get(), database);

    // The scanner asks this before every file. Routing it through the server means
    // the warm-up yields to any active transfer rather than competing with it.
    m_scanner->setBusyPredicate([this] {
        return m_server->activeStreams() > 0;
    });

    qInfo() << "[Thumbs] Cache at" << cacheDirectory
            << "| format" << m_thumbnails->outputFormat()
            << "| generators" << detected.thumbnailThreads;
}

void FileServer::setAccountStore(db::Database* database)
{
    if (!database)
        return;

    m_users    = std::make_unique<auth::UserStore>(database);
    m_sessions = std::make_unique<auth::SessionStore>(database);
    m_limiter  = std::make_unique<auth::LoginRateLimiter>();

    m_authentication = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());

    m_router->setAuthHandler(m_authentication.get());

    // A server left running for weeks would otherwise only ever sweep at startup.
    // Hourly is far more often than needed for a handful of users, and costs one
    // indexed delete.
    m_sessionSweeper = std::make_unique<QTimer>();
    m_sessionSweeper->setInterval(60 * 60 * 1000);
    QObject::connect(m_sessionSweeper.get(), &QTimer::timeout, this, [this]() {
        const int removed = m_sessions->sweepExpired();
        if (removed > 0)
            qInfo() << "[Auth] Retired" << removed << "expired sessions.";
    });
    m_sessionSweeper->start();

    qInfo() << "[Auth] Accounts on record:" << m_users->count();

    // Said plainly because it is the difference between "has a login page" and "is
    // protected". Step 14 adds the gate that makes the session actually required.
    if (m_users->count() == 0) {
        qWarning() << "[Auth] No accounts exist yet. The login page is served, but "
                      "no route requires a session — do not expose this beyond a "
                      "trusted network.";
    }
}

int FileServer::accountCount() const
{
    return m_users ? m_users->count() : 0;
}

void FileServer::setThumbnailWarmupEnabled(bool enabled)
{
    m_warmupEnabled = enabled;

    if (!enabled && m_scanner)
        m_scanner->stop();
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

    // Started after listening, so the scanner's busy check sees a live server and
    // yields immediately if someone is already transferring.
    if (m_warmupEnabled && m_scanner) {
        m_scanner->setRoot(m_router->nasRoot());
        m_scanner->start();
    }

    emit serverStarted(localIP(), m_port);
    return true;
}

void FileServer::stop()
{
    if (!m_server->isListening())
        return;

    // Stopped first: there is no point warming a cache nobody can read from, and
    // the scan would otherwise keep a disk busy after the user stopped the server.
    if (m_scanner)
        m_scanner->stop();

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
