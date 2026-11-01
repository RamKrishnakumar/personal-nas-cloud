#pragma once

#include <QDateTime>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>

#include <memory>

class QTimer;

namespace core {

class Tailscale;

/**
 * @class CertRenewer
 * @brief Keeps the server's certificate from expiring, without a restart.
 *
 * ### Why the application and not a scheduler
 *
 * A certificate from a real authority lives 90 days, so something has to renew it.
 * The platform-native answer is three separate mechanisms — a Task Scheduler entry
 * on Windows, a launchd plist on macOS, a systemd timer on Linux — each needing an
 * installer to register it and each failing differently. A QTimer in a process that
 * is meant to run continuously anyway is one code path on all three, needs no
 * installer support, and can hand the new certificate straight to the running server
 * instead of requiring a restart to pick it up.
 *
 * The trade is that nothing renews while the application is closed. That is
 * acceptable: the check runs on start() as well as on the timer, so a machine that
 * was off for a month renews on its next launch, before anyone tries to connect.
 *
 * ### Why the timer is daily rather than tuned
 *
 * `tailscale cert` returns the certificate it already has until that certificate is
 * close to expiry, and only then talks to Let's Encrypt. So a check that finds
 * nothing to do is nearly free, and polling daily needs no coordination with the
 * issuer's own renewal window — which is the sort of thing that would otherwise
 * drift out of agreement after an upstream change nobody here would notice.
 */
class CertRenewer : public QObject
{
    Q_OBJECT

public:
    /** How long before expiry renewal is attempted. */
    static constexpr int DefaultThresholdDays = 14;

    /** Gap between checks. See the class note on why this is not tuned. */
    static constexpr int CheckIntervalMs = 24 * 60 * 60 * 1'000;

    /**
     * @param tailscale Must outlive this object. Not owned.
     * @param certificatePath Where the certificate is written and read from.
     * @param privateKeyPath Likewise for the key.
     */
    CertRenewer(Tailscale*     tailscale,
                const QString& certificatePath,
                const QString& privateKeyPath,
                QObject*       parent = nullptr);
    ~CertRenewer() override;

    /**
     * @brief The whole renewal policy, as a function of two instants.
     *
     * Separated out and made static because it is the only part with a decision in
     * it, and because testing it this way needs two QDateTimes rather than a
     * certificate authority. Everything around it is plumbing.
     *
     * An invalid @p expiry renews: either the certificate could not be read or it
     * carries no expiry, and attempting to replace something unreadable is better
     * than serving it until a browser refuses it.
     *
     * @param thresholdDays Values at or below zero fall back to
     *        DefaultThresholdDays, so a misconfigured certRenewDays cannot disable
     *        renewal altogether and silently let the certificate lapse.
     */
    static bool shouldRenew(const QDateTime& expiry,
                            const QDateTime& now,
                            int              thresholdDays);

    /**
     * @brief Whether @p certificate is actually valid for @p dnsName.
     *
     * The other half of the renewal decision, and the half that was missing. Asking
     * only whether a certificate is close to expiry is not enough: the self-signed
     * certificate scripts/gen-cert.sh writes is valid for years, so a renewer that
     * looks only at dates concludes there is nothing to do and never requests the
     * real certificate at all. The server then serves a name no browser will accept
     * for the address being used — which is precisely what an iPhone reported.
     *
     * ### Subject alternative names only, never the common name
     *
     * Browsers have ignored the common name for years; RFC 2818 deprecated it and
     * every current engine validates against the SAN extension alone. So a
     * certificate whose only claim to a name is its CN is useless in practice, and
     * treating it as *not* covering the name is what makes this agree with Safari.
     * A certificate with no SAN extension covers nothing.
     *
     * Matching is case-insensitive, as DNS is. A single leading `*.` wildcard
     * matches exactly one label, so `*.example.com` covers `a.example.com` but not
     * `example.com` or `a.b.example.com` — the rule browsers apply.
     */
    static bool covers(const QSslCertificate& certificate, const QString& dnsName);

    void setThresholdDays(int days);
    int  thresholdDays() const { return m_thresholdDays; }

    /**
     * @brief Sets the name to request a certificate for.
     *
     * Normally the MagicDNS name from Tailscale::status(). Renewal is skipped while
     * this is empty, which is the logged-out case.
     */
    void setDnsName(const QString& dnsName);
    QString dnsName() const { return m_dnsName; }

    /**
     * @brief The leaf certificate currently on disk, or a null certificate.
     *
     * The leaf is the first in the file by PEM convention, and the one both halves
     * of the renewal decision are about: it holds the names, and it expires soonest.
     */
    QSslCertificate currentLeaf() const;

    /** Expiry of the certificate currently on disk, or invalid if unreadable. */
    QDateTime currentExpiry() const;

    /** Reason the last attempt failed, or empty. */
    QString lastError() const { return m_lastError; }

    /** Checks once immediately, then daily. */
    void start();
    void stop();

    /**
     * @brief Renews now if the certificate is due, and reloads it if it was renewed.
     *
     * @return true when a new certificate was obtained and loaded.
     */
    bool checkNow();

signals:
    /**
     * @brief A new certificate is ready to serve.
     *
     * FileServer connects this to its own reload, which swaps the configuration
     * without disturbing transfers already in progress.
     */
    void certificateRenewed(QSslConfiguration configuration);

    /** Renewal was due but did not succeed. Carries the reason for the UI. */
    void renewalFailed(QString reason);

private:
    Tailscale* m_tailscale = nullptr;

    QString m_certificatePath;
    QString m_privateKeyPath;
    QString m_dnsName;
    QString m_lastError;

    int m_thresholdDays = DefaultThresholdDays;

    std::unique_ptr<QTimer> m_timer;
};

} // namespace core
