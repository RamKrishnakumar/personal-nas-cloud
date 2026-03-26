#include "DdnsUpdater.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────
// External service used to discover the current WAN IP.
// ipify.org is a free, stable, widely-used public IP detection API.
// Returns a plain text IPv4 address e.g. "117.247.29.111"
// ─────────────────────────────────────────────────────────────────────────────
static const QString WAN_IP_SERVICE = "https://api.ipify.org?format=text";

// ─────────────────────────────────────────────────────────────────────────────
// No-IP's official DDNS update endpoint.
// Uses DynDNS protocol v2 — same format used by most routers and DDNS clients.
// Reference: https://www.noip.com/integrate/request
// ─────────────────────────────────────────────────────────────────────────────
static const QString NOIP_UPDATE_URL = "https://dynupdate.no-ip.com/nic/update";

// ─────────────────────────────────────────────────────────────────────────────
// No-IP requires a descriptive User-Agent identifying your app.
// Format: "AppName/Version contact@email.com"
// Without this, No-IP may block requests as anonymous/bot traffic.
// ─────────────────────────────────────────────────────────────────────────────
static const QByteArray USER_AGENT = "NestDrive/0.4 nestdrive-app@example.com";

// ─────────────────────────────────────────────────────────────────────────────

DdnsUpdater::DdnsUpdater(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    // Wire the timer's timeout signal to our updateNow() slot.
    // Every time the timer fires, we run a fresh IP check cycle.
    connect(m_timer, &QTimer::timeout, this, &DdnsUpdater::updateNow);
}

// ─────────────────────────────────────────────────────────────────────────────

void DdnsUpdater::setCredentials(const QString& username,
                                 const QString& password,
                                 const QString& hostname)
{
    m_username = username;
    m_password = password;
    m_hostname = hostname;

    qDebug() << "[DDNS] Credentials set for hostname:" << hostname;
}

// ─────────────────────────────────────────────────────────────────────────────

void DdnsUpdater::start(int intervalMs)
{
    qDebug() << "[DDNS] Starting updater. Check interval:"
             << intervalMs / 1000 << "seconds";

    // Start the repeating timer — fires every intervalMs milliseconds
    m_timer->start(intervalMs);

    // Run an immediate check right now instead of waiting
    // for the first timer tick (which could be 5 mins away)
    updateNow();
}

// ─────────────────────────────────────────────────────────────────────────────

void DdnsUpdater::stop()
{
    qDebug() << "[DDNS] Updater stopped.";
    m_timer->stop();
    // m_lastWanIp is intentionally preserved so next start()
    // doesn't fire a redundant update if IP hasn't changed
}

// ─────────────────────────────────────────────────────────────────────────────

void DdnsUpdater::updateNow()
{
    qDebug() << "[DDNS] Checking current WAN IP via ipify.org...";

    // ── Step 1: Fetch current WAN IP from ipify.org ───────────────────────────
    // FIX: Split into two lines to avoid C++ "Most Vexing Parse".
    // Writing QNetworkRequest req(QUrl(WAN_IP_SERVICE)) in one line makes the
    // compiler think 'req' is a function declaration, not a variable.
    // Two named variables remove all ambiguity.
    QUrl wanIpUrl(WAN_IP_SERVICE);          // Build the URL object first
    QNetworkRequest req(wanIpUrl);          // Then pass it to QNetworkRequest

    // Send GET request — reply is a heap object owned by Qt, we must deleteLater()
    auto* reply = m_net.get(req);

    // Qt calls this lambda automatically when ipify.org responds.
    // 'reply' is explicitly captured so the lambda can read data and clean up.
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {

        // Guard: check for network errors before reading data
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "[DDNS] Failed to fetch WAN IP:" << reply->errorString();
            emit updateFailed("WAN IP fetch failed: " + reply->errorString());
            reply->deleteLater(); // Always clean up Qt network reply objects
            return;
        }

        // Read the plain text IP from ipify's response body
        QString wanIp = QString::fromUtf8(reply->readAll()).trimmed();
        reply->deleteLater(); // Done reading — schedule deletion

        if (wanIp.isEmpty()) {
            qDebug() << "[DDNS] ipify returned empty response.";
            emit updateFailed("Empty WAN IP response from ipify.org");
            return;
        }

        qDebug() << "[DDNS] Current WAN IP:" << wanIp;

        // ── Step 2: Compare with last known IP ────────────────────────────────
        // If IP hasn't changed, skip the No-IP update entirely.
        // This avoids unnecessary API calls and prevents rate limiting.
        if (wanIp == m_lastWanIp) {
            qDebug() << "[DDNS] IP unchanged (" << wanIp << ") — skipping update.";
            return;
        }

        // IP has changed — cache the new one and push update to No-IP
        qDebug() << "[DDNS] IP changed from" << m_lastWanIp
                 << "to" << wanIp << "— sending update to No-IP...";
        m_lastWanIp = wanIp;

        // ── Step 3: Build the No-IP update URL with query parameters ──────────
        // Final URL format: /nic/update?hostname=nestdrive.ddns.org&myip=1.2.3.4
        QUrl updateUrl(NOIP_UPDATE_URL);
        QUrlQuery query;
        query.addQueryItem("hostname", m_hostname);
        query.addQueryItem("myip",     wanIp);
        updateUrl.setQuery(query);

        // Same fix applied here too — named variable avoids vexing parse
        QNetworkRequest updateReq(updateUrl);

        // ── Step 4: Build the Basic Auth header ───────────────────────────────
        // No-IP uses HTTP Basic Auth: Authorization: Basic Base64("user:pass")
        // Base64 is NOT encryption — it's just encoding. The real security
        // comes from HTTPS (TLS) encrypting the entire request in transit.
        QByteArray credentials = (m_username + ":" + m_password).toUtf8();
        QByteArray authHeader  = "Basic " + credentials.toBase64();

        updateReq.setRawHeader("Host",          "dynupdate.no-ip.com");
        updateReq.setRawHeader("Authorization", authHeader);
        updateReq.setRawHeader("User-Agent",    USER_AGENT);

        // ── Step 5: Send the update request to No-IP ─────────────────────────
        auto* updateReply = m_net.get(updateReq);
        connect(updateReply, &QNetworkReply::finished,
                this, [this, updateReply, wanIp]() {

                    if (updateReply->error() != QNetworkReply::NoError) {
                        qDebug() << "[DDNS] No-IP request failed:"
                                 << updateReply->errorString();
                        emit updateFailed("No-IP update failed: "
                                          + updateReply->errorString());
                        updateReply->deleteLater();
                        return;
                    }

                    // ── Step 6: Parse No-IP's response string ─────────────────────────
                    // "good 1.2.3.4"  → success, IP was updated
                    // "nochg 1.2.3.4" → IP already correct, no change needed (still OK)
                    // "nohost"         → hostname not found in your No-IP account
                    // "badauth"        → wrong username or password
                    // "abuse"          → account flagged, update requests being blocked
                    QString response = QString::fromUtf8(updateReply->readAll()).trimmed();
                    updateReply->deleteLater();

                    qDebug() << "[DDNS] No-IP response:" << response;

                    if (response.startsWith("good") || response.startsWith("nochg")) {
                        // Both are successful — hostname is pointing to the correct IP
                        emit updateSucceeded(wanIp, response);
                    } else {
                        // Any other response is an error — log it for debugging
                        emit updateFailed("No-IP returned: " + response);
                    }
                });
    });
}
