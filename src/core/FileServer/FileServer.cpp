#include "core/FileServer/FileServer.h"

#include "auth/LoginRateLimiter.h"
#include "auth/PermissionStore.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "db/AuditLog.h"
#include "handlers/AuthGate.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/HardwareTier.h"
#include "http/HttpServer.h"
#include "media/FrameExtractor.h"
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
    m_sessionSweeper.reset();
    m_gate.reset();             // Points at the router and the stores below it.
    m_router.reset();
    m_scanner.reset();      // Joins its thread before the cache it uses goes away.
    m_thumbnails.reset();
    m_frames.reset();       // Held by the cache above.

    m_authentication.reset();   // Joins its hashing thread.
    m_limiter.reset();

    // After the router and the auth handler, both of which hold a raw pointer to it.
    m_audit.reset();

    m_folderPerms.reset();
    m_sessions.reset();
    m_users.reset();
}

bool FileServer::setFrameExtractor(const QString& executableOverride)
{
    m_frames = std::make_unique<media::FrameExtractor>(executableOverride);

    // Handed over here as well, covering the case where this is called after the
    // store was already built. Without it the order of the two calls would silently
    // decide whether videos get thumbnails.
    if (m_thumbnails)
        m_thumbnails->setFrameExtractor(m_frames.get());

    return m_frames->isAvailable();
}

QString FileServer::frameExtractorVersion() const
{
    return m_frames ? m_frames->versionLine() : QString();
}

void FileServer::setThumbnailStore(db::Database* database, const QString& cacheDirectory)
{
    // Generator threads come from the same tier detection as everything else, but
    // the profile is only established in start(). One thread is the conservative
    // choice and matches the Light tier, which is the target hardware.
    const http::HardwareProfile detected = http::detectHardwareProfile();

    m_thumbnails = std::make_unique<media::ThumbnailCache>(
        database, cacheDirectory, detected.thumbnailThreads);

    // Before the router and the scanner see the cache, because both ask it what it
    // can generate for and the answer depends on this.
    if (m_frames)
        m_thumbnails->setFrameExtractor(m_frames.get());

    m_router->setThumbnailCache(m_thumbnails.get());

    m_scanner = std::make_unique<media::ThumbnailScanner>(m_thumbnails.get(), database);

    // The scanner asks this before every file. Routing it through the server means
    // the warm-up yields to any active transfer rather than competing with it.
    m_scanner->setBusyPredicate([this] {
        return m_server->activeStreams() > 0;
    });

    qInfo() << "[Thumbs] Cache at" << cacheDirectory
            << "| format" << m_thumbnails->outputFormat()
            << "| generators" << detected.thumbnailThreads
            << "| video and HEIC"
            << ((m_frames && m_frames->isAvailable()) ? "on" : "off (no ffmpeg)");
}

void FileServer::setAccountStore(db::Database* database)
{
    if (!database)
        return;

    m_users       = std::make_unique<auth::UserStore>(database);
    m_sessions    = std::make_unique<auth::SessionStore>(database);
    m_folderPerms = std::make_unique<auth::PermissionStore>(database);
    m_limiter     = std::make_unique<auth::LoginRateLimiter>();
    m_audit       = std::make_unique<db::AuditLog>(database);

    m_authentication = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());
    m_authentication->setAuditLog(m_audit.get());
    m_router->setAuditLog(m_audit.get());

    // The gate becomes the server's router, with the file router behind it. Nothing
    // reaches the library without passing through here.
    m_gate = std::make_unique<handlers::AuthGate>(
        m_router.get(), m_authentication.get(),
        m_sessions.get(), m_users.get(), m_folderPerms.get());

    if (!m_server->setRouter(m_gate.get())) {
        // Only possible if this was called after start(), which the header forbids.
        // Said loudly because the consequence is a server with no gate.
        qCritical() << "[Auth] ❌ Could not install the authentication gate: the "
                       "server is already listening. Stop it and reconfigure.";
        m_gate.reset();
        return;
    }

    // A server left running for weeks would otherwise only ever sweep at startup.
    // Hourly is far more often than needed for a handful of users, and costs one
    // indexed delete.
    m_sessionSweeper = std::make_unique<QTimer>();
    m_sessionSweeper->setInterval(60 * 60 * 1000);
    QObject::connect(m_sessionSweeper.get(), &QTimer::timeout, this, [this]() {
        const int removed = m_sessions->sweepExpired();
        if (removed > 0)
            qInfo() << "[Auth] Retired" << removed << "expired sessions.";

        // Shares the timer because it is the same shape of job and wants the same
        // cadence. The audit table is the only one in the database with no natural
        // ceiling: every folder opened adds a row, so without this it is the part
        // that eventually makes the file large.
        const int expired = m_audit->sweepOlderThan(db::AuditLog::DefaultRetentionDays);
        if (expired > 0)
            qInfo() << "[Audit] Discarded" << expired << "entries past retention.";
    });
    m_sessionSweeper->start();

    // Once at startup as well as hourly. A machine that is only ever on for an hour
    // at a time would otherwise never reach the first tick, and would keep entries
    // for ever.
    if (const int expired = m_audit->sweepOlderThan(db::AuditLog::DefaultRetentionDays);
        expired > 0) {
        qInfo() << "[Audit] Discarded" << expired << "entries past retention.";
    }

    qInfo() << "[Auth] Accounts on record:" << m_users->count();

    // With the gate in place and no accounts, nobody can get in at all. That is the
    // safe direction, but it needs saying or it looks like the server is broken.
    if (m_users->count() == 0) {
        qWarning() << "[Auth] No accounts exist. Every request will be redirected to "
                      "the login page, and no login can succeed until an account is "
                      "created from the desktop window.";
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
