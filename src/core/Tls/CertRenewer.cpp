#include "core/Tls/CertRenewer.h"

#include "core/Tls/Tailscale.h"
#include "core/Tls/TlsConfig.h"

#include <QDebug>
#include <QSslCertificate>
#include <QTimer>

namespace core {

// resolveTlsPath() comes from TlsConfig.h rather than being repeated here, so that
// this and the loader cannot resolve the same configured path to different files.

CertRenewer::CertRenewer(Tailscale*     tailscale,
                         const QString& certificatePath,
                         const QString& privateKeyPath,
                         QObject*       parent)
    : QObject(parent)
    , m_tailscale(tailscale)
    , m_certificatePath(certificatePath)
    , m_privateKeyPath(privateKeyPath)
    , m_timer(std::make_unique<QTimer>())
{
    m_timer->setInterval(CheckIntervalMs);
    connect(m_timer.get(), &QTimer::timeout, this, [this]() { checkNow(); });
}

CertRenewer::~CertRenewer() = default;

bool CertRenewer::shouldRenew(const QDateTime& expiry,
                              const QDateTime& now,
                              int              thresholdDays)
{
    const int threshold = thresholdDays > 0 ? thresholdDays : DefaultThresholdDays;

    // Unreadable or undated: renew. Replacing something we cannot reason about beats
    // serving it until a browser rejects it, and the cost of being wrong is one
    // wasted CLI call that returns the cached certificate anyway.
    if (!expiry.isValid())
        return true;

    return now.addDays(threshold) >= expiry;
}

bool CertRenewer::covers(const QSslCertificate& certificate, const QString& dnsName)
{
    if (certificate.isNull() || dnsName.isEmpty())
        return false;

    const QList<QString> names =
        certificate.subjectAlternativeNames().values(QSsl::DnsEntry);

    const QString wanted = dnsName.toLower();

    for (const QString& entry : names) {
        const QString candidate = entry.trimmed().toLower();
        if (candidate.isEmpty())
            continue;

        if (candidate == wanted)
            return true;

        if (!candidate.startsWith(QLatin1String("*.")))
            continue;

        // A wildcard covers exactly one label, which is the rule browsers apply:
        // *.example.com matches a.example.com but neither example.com itself nor
        // a.b.example.com. Comparing suffixes alone would wrongly accept the last of
        // those, and a certificate we wrongly think covers the name is worse than one
        // we wrongly re-request — it produces the browser warning this exists to
        // prevent.
        const QString suffix = candidate.mid(1);        // ".example.com"
        if (!wanted.endsWith(suffix))
            continue;

        const QString label = wanted.left(wanted.size() - suffix.size());
        if (!label.isEmpty() && !label.contains(QLatin1Char('.')))
            return true;
    }

    return false;
}

void CertRenewer::setThresholdDays(int days)
{
    if (days > 0)
        m_thresholdDays = days;
}

void CertRenewer::setDnsName(const QString& dnsName)
{
    m_dnsName = dnsName;
}

QSslCertificate CertRenewer::currentLeaf() const
{
    const QList<QSslCertificate> chain =
        QSslCertificate::fromPath(resolveTlsPath(m_certificatePath), QSsl::Pem);

    // The leaf, which is first by PEM convention. It is the one that carries the
    // names and the one that expires soonest in practice; an intermediate outliving
    // it is normal and not something renewal should be timed against.
    // Named rather than `return {}`: QSslCertificate's default constructor is
    // explicit, so copy-list-initialisation cannot select it. GCC accepts the braces
    // anyway; MSVC is right to refuse them.
    if (chain.isEmpty())
        return QSslCertificate();

    return chain.first();
}

QDateTime CertRenewer::currentExpiry() const
{
    const QSslCertificate leaf = currentLeaf();
    return leaf.isNull() ? QDateTime() : leaf.expiryDate();
}

void CertRenewer::start()
{
    checkNow();
    m_timer->start();
}

void CertRenewer::stop()
{
    m_timer->stop();
}

bool CertRenewer::checkNow()
{
    if (!m_tailscale || !m_tailscale->isAvailable()) {
        // Not an error worth reporting. Tailscale being absent is an ordinary
        // configuration — the server runs on the LAN with its self-signed
        // certificate exactly as before.
        return false;
    }

    if (m_dnsName.isEmpty()) {
        m_lastError = QStringLiteral("Tailscale is installed but this node is logged "
                                     "out, so no certificate can be issued.");
        return false;
    }

    // Two independent reasons to renew, and only one of them is about time.
    //
    // The name test is the one that was missing, and its absence was the whole of a
    // real failure: the self-signed certificate from scripts/gen-cert.sh is valid
    // until 2029, so the expiry test alone answered "nothing to do" and the real
    // certificate was never requested. The server went on presenting a certificate
    // for nestdrive.local while being reached at a .ts.net name, and Safari refused
    // it — correctly.
    const QSslCertificate leaf = currentLeaf();

    const bool wrongName = !covers(leaf, m_dnsName);
    const bool expiring =
        shouldRenew(leaf.isNull() ? QDateTime() : leaf.expiryDate(),
                    QDateTime::currentDateTimeUtc(),
                    m_thresholdDays);

    if (!wrongName && !expiring) {
        // Logged rather than returned silently. The silent early return is exactly
        // what made the bug above invisible: nothing was wrong in the log, nothing
        // was wrong in the window, and the only symptom was a warning on a phone.
        qInfo() << "[TLS] Certificate is current and covers" << m_dnsName
                << "— expires" << leaf.expiryDate().toString(Qt::ISODate);
        return false;
    }

    const QString certFilePath = resolveTlsPath(m_certificatePath);
    const QString keyFilePath  = resolveTlsPath(m_privateKeyPath);

    qInfo() << "[TLS] Requesting a certificate for" << m_dnsName << "—"
            << (wrongName ? "the one on disk does not cover that name"
                          : "the one on disk is close to expiry");

    QString error;
    if (!m_tailscale->issueCert(m_dnsName, certFilePath, keyFilePath, &error)) {
        m_lastError = error;
        qWarning() << "[TLS] ⚠️ Renewal failed:" << error;
        emit renewalFailed(error);
        return false;
    }

    // Load it before announcing it. Emitting a configuration that turns out not to
    // parse would replace a working certificate on every new connection with one
    // that cannot complete a handshake — the one failure mode worse than an expired
    // certificate, because it has no warning page to click through.
    const std::optional<QSslConfiguration> configuration =
        loadTls(certFilePath, keyFilePath, &error);

    if (!configuration) {
        m_lastError = error;
        qWarning() << "[TLS] ⚠️ Renewed certificate could not be loaded:" << error;
        emit renewalFailed(error);
        return false;
    }

    m_lastError.clear();

    qInfo() << "[TLS] ✅ Certificate renewed; expires"
            << currentExpiry().toString(Qt::ISODate);

    emit certificateRenewed(*configuration);
    return true;
}

} // namespace core
