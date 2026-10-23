#include "core/Net/ConnectionCheck.h"

#include "core/Net/Upnp.h"

#include <QDebug>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace core {

const char* const PublicAddressService = "https://api.ipify.org?format=text";

namespace {

/// Milliseconds allowed for the public-address lookup.
constexpr int PublicAddressTimeoutMs = 8'000;

/**
 * @brief True for the range RFC 6598 reserves for carrier-grade NAT.
 *
 * A weak signal on its own — providers do not have to use it, and the one this was
 * developed against does not — but conclusive when it does match: an address in here
 * is never routable on the public internet.
 */
bool isReservedForCarrierNat(const QString& address)
{
    const QHostAddress parsed(address);
    if (parsed.protocol() != QAbstractSocket::IPv4Protocol)
        return false;

    return parsed.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
}

} // namespace

QString describeConnectionType(ConnectionType type)
{
    switch (type) {
    case ConnectionType::PublicIp:
        return QObject::tr("Public IP address");
    case ConnectionType::CarrierNat:
        return QObject::tr("Behind the provider's NAT");
    case ConnectionType::Unknown:
        break;
    }

    return QObject::tr("Not determined");
}

QString connectionTypeToString(ConnectionType type)
{
    switch (type) {
    case ConnectionType::PublicIp:
        return QStringLiteral("public");
    case ConnectionType::CarrierNat:
        return QStringLiteral("carrierNat");
    case ConnectionType::Unknown:
        break;
    }

    return QStringLiteral("unknown");
}

ConnectionType connectionTypeFromString(const QString& text)
{
    const QString value = text.trimmed().toLower();

    if (value == QLatin1String("public"))
        return ConnectionType::PublicIp;

    if (value == QLatin1String("carriernat"))
        return ConnectionType::CarrierNat;

    return ConnectionType::Unknown;
}

ConnectionType classifyConnection(const QString& routerWanIp, const QString& publicIp)
{
    // Without a public address there is nothing to compare and nothing to conclude.
    // Saying so beats guessing, because every wrong guess here sends the owner down
    // a setup path that cannot work.
    if (publicIp.trimmed().isEmpty())
        return ConnectionType::Unknown;

    if (routerWanIp.trimmed().isEmpty()) {
        // No answer from the router. The reserved range still settles it when it
        // matches; otherwise this is genuinely unknown, and claiming otherwise is
        // what the range check alone gets wrong on providers using squat space.
        return isReservedForCarrierNat(publicIp) ? ConnectionType::CarrierNat
                                                 : ConnectionType::Unknown;
    }

    // The direct question, and the reliable answer: the router either holds the
    // address the internet sees, or something between them does.
    return routerWanIp.trimmed() == publicIp.trimmed() ? ConnectionType::PublicIp
                                                       : ConnectionType::CarrierNat;
}

ConnectionCheck::ConnectionCheck(UpnpClient* upnp, QObject* parent)
    : QObject(parent)
    , m_upnp(upnp)
    , m_net(std::make_unique<QNetworkAccessManager>())
{
}

ConnectionCheck::~ConnectionCheck() = default;

void ConnectionCheck::start()
{
    m_routerWanIp.clear();
    m_publicIp.clear();
    m_routerDone = false;
    m_publicDone = false;

    fetchPublicAddress();

    if (!m_upnp) {
        m_routerDone = true;
        conclude();
        return;
    }

    // Both run at once. They are independent, and running them in sequence would
    // make the window wait for the sum of two timeouts instead of the larger.
    connect(m_upnp, &UpnpClient::externalAddressReceived, this,
            [this](const QString& address) {
        m_routerWanIp = address;
        m_routerDone  = true;
        conclude();
    });

    connect(m_upnp, &UpnpClient::failed, this, [this](const QString& reason) {
        qDebug() << "[Connection] Router would not report its address:" << reason;
        m_routerDone = true;
        conclude();
    });

    connect(m_upnp, &UpnpClient::discoveryFinished, this, [this](bool found) {
        if (found) {
            m_upnp->requestExternalAddress();
            return;
        }

        m_routerDone = true;
        conclude();
    });

    m_upnp->discover();
}

void ConnectionCheck::fetchPublicAddress()
{
    QNetworkRequest request(QUrl(QString::fromLatin1(PublicAddressService)));
    request.setTransferTimeout(PublicAddressTimeoutMs);

    QNetworkReply* reply = m_net->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            const QString body = QString::fromUtf8(reply->readAll()).trimmed();

            // Validated rather than trusted. A captive portal or a provider error
            // page answers 200 with HTML, and treating that as an address would make
            // the comparison below nonsense.
            if (!QHostAddress(body).isNull())
                m_publicIp = body;
            else
                qDebug() << "[Connection] Public address service returned no address.";
        } else {
            qDebug() << "[Connection] Public address lookup failed:"
                     << reply->errorString();
        }

        m_publicDone = true;
        conclude();
    });
}

void ConnectionCheck::conclude()
{
    if (!m_routerDone || !m_publicDone)
        return;

    m_result = classifyConnection(m_routerWanIp, m_publicIp);

    qInfo().noquote() << "[Connection]" << explanation();

    emit finished(m_result);
}

QString ConnectionCheck::explanation() const
{
    if (m_publicIp.isEmpty())
        return tr("Could not reach the internet to check the public address.");

    if (m_routerWanIp.isEmpty()) {
        if (m_result == ConnectionType::CarrierNat) {
            return tr("The internet sees %1, which is a range reserved for "
                      "provider-level NAT. Forwarding a port cannot work.")
                .arg(m_publicIp);
        }

        return tr("The internet sees %1. The router would not report its own "
                  "address, so this is unconfirmed — compare that number against "
                  "the WAN address on the router's admin page. If they differ, "
                  "forwarding a port cannot work.")
            .arg(m_publicIp);
    }

    if (m_result == ConnectionType::PublicIp) {
        return tr("The router holds %1, which is also what the internet sees. "
                  "Forwarding a port will work.")
            .arg(m_publicIp);
    }

    return tr("The router holds %1 but the internet sees %2. The provider operates "
              "a NAT in between, so forwarding a port cannot work — nothing on the "
              "internet can address the router.")
        .arg(m_routerWanIp, m_publicIp);
}

} // namespace core
