#include "core/Tls/TlsConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QStringList>

namespace core {

QString resolveTlsPath(const QString& path)
{
    return QDir::isAbsolutePath(path)
               ? path
               : QCoreApplication::applicationDirPath() + QLatin1Char('/') + path;
}

namespace {

/**
 * @brief Reads a private key without being told its algorithm.
 *
 * QSslKey requires the algorithm up front and will not sniff it, so the only way to
 * accept whatever the certificate authority issued is to try each in turn. EC comes
 * first because it is what `tailscale cert` and most current ACME clients produce;
 * RSA second because that is what scripts/gen-cert.sh still emits for LAN use.
 *
 * The device is rewound between attempts. Without that, the second attempt reads
 * from the end of the file and fails for a reason that has nothing to do with the
 * algorithm — which is a genuinely confusing bug to find, since the first file in a
 * test run passes and every later one does not.
 */
QSslKey readKeyOfAnyAlgorithm(QIODevice* device)
{
    for (const QSsl::KeyAlgorithm algorithm : { QSsl::Ec, QSsl::Rsa, QSsl::Dsa }) {
        device->seek(0);

        const QSslKey key(device, algorithm, QSsl::Pem, QSsl::PrivateKey);
        if (!key.isNull())
            return key;
    }

    return {};
}

} // namespace

std::optional<QSslConfiguration> loadTls(const QString& certificatePath,
                                         const QString& privateKeyPath,
                                         QString*       error)
{
    const auto fail = [error](const QString& reason) -> std::optional<QSslConfiguration> {
        if (error)
            *error = reason;
        return std::nullopt;
    };

    const QString certFilePath = resolveTlsPath(certificatePath);
    const QString keyFilePath  = resolveTlsPath(privateKeyPath);

    QFile certFile(certFilePath);
    if (!certFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Certificate file not found: %1").arg(certFilePath));

    // The whole chain, not just the leaf. fromDevice() returns them in file order,
    // and the PEM convention every CA follows is leaf first, then the intermediates
    // that vouch for it — which is also the order setLocalCertificateChain() wants.
    const QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();

    if (chain.isEmpty() || chain.first().isNull())
        return fail(QStringLiteral("Certificate is not valid PEM: %1").arg(certFilePath));

    QFile keyFile(keyFilePath);
    if (!keyFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Private key file not found: %1").arg(keyFilePath));

    const QSslKey privateKey = readKeyOfAnyAlgorithm(&keyFile);
    keyFile.close();

    if (privateKey.isNull()) {
        // Deliberately does not name an algorithm. The old message said "not a valid
        // RSA PEM key", which was actively misleading for the EC keys it was
        // rejecting: it read as a complaint about the file rather than about this
        // code's refusal to consider anything else.
        return fail(QStringLiteral("Private key is not a readable unencrypted PEM key "
                                   "(tried EC, RSA and DSA): %1")
                        .arg(keyFilePath));
    }

    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificateChain(chain);
    configuration.setPrivateKey(privateKey);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);

    // This server does not ask clients for certificates; authentication is the
    // session cookie's job. Verifying a peer that was never asked to present
    // anything would reject every browser.
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);

    if (error)
        error->clear();

    return configuration;
}

QString describeTlsBackend()
{
    if (!QSslSocket::supportsSsl())
        return QStringLiteral("none — this build cannot serve HTTPS");

    const QString active    = QSslSocket::activeBackend();
    const QStringList avail = QSslSocket::availableBackends();

    return QStringLiteral("%1 (available: %2) %3")
        .arg(active,
             avail.join(QStringLiteral(", ")),
             QSslSocket::sslLibraryVersionString());
}

} // namespace core
