#include "core/Net/Upnp.h"

#include <QDebug>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUdpSocket>
#include <QXmlStreamReader>

namespace core {
namespace {

/// The SSDP multicast group and port, fixed by the specification.
const QHostAddress SsdpGroup(QStringLiteral("239.255.255.250"));

/**
 * Service types we can use, best first.
 *
 * WANPPPConnection is last but not optional: it is what DSL routers expose, and this
 * household's connection is PPPoE.
 */
constexpr const char* WantedServices[] = {
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
};

QByteArray searchRequest(const char* deviceType)
{
    // MX is the most seconds a router may wait before replying, which it is meant to
    // randomise within to avoid every device on the network answering at once. Two,
    // against our three-second timeout.
    return QByteArray("M-SEARCH * HTTP/1.1\r\n"
                      "HOST: 239.255.255.250:1900\r\n"
                      "MAN: \"ssdp:discover\"\r\n"
                      "MX: 2\r\n"
                      "ST: ")
           + deviceType + "\r\n\r\n";
}

} // namespace

// ── Parsing ─────────────────────────────────────────────────────────────────

namespace upnp {

QUrl parseSsdpLocation(const QByteArray& reply)
{
    // Case-insensitive, because SSDP only looks like HTTP. Routers in the wild send
    // LOCATION, Location and location, and a case-sensitive match silently finds
    // nothing on whichever ones disagree with the guess.
    for (const QByteArray& line : reply.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;

        if (line.left(colon).trimmed().toLower() != QByteArrayLiteral("location"))
            continue;

        const QUrl url(QString::fromLatin1(line.mid(colon + 1).trimmed()));
        if (url.isValid() && !url.host().isEmpty())
            return url;
    }

    return {};
}

UpnpService parseDeviceDescription(const QByteArray& xml, const QUrl& baseUrl)
{
    QHash<QString, QString> controlUrls;   // serviceType -> controlURL, as written
    QUrl                    declaredBase;

    QString currentServiceType;
    QString currentControlUrl;
    bool    insideService = false;

    QXmlStreamReader reader(xml);

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement()) {
            const QString name = reader.name().toString();

            if (name == QLatin1String("service")) {
                insideService     = true;
                currentServiceType.clear();
                currentControlUrl.clear();
            } else if (name == QLatin1String("URLBase")) {
                // Overrides baseUrl for resolving relative control URLs. Honouring
                // it is what makes this work on routers that serve their description
                // from one port and their control endpoint on another.
                declaredBase = QUrl(reader.readElementText().trimmed());
            } else if (insideService && name == QLatin1String("serviceType")) {
                currentServiceType = reader.readElementText().trimmed();
            } else if (insideService && name == QLatin1String("controlURL")) {
                currentControlUrl = reader.readElementText().trimmed();
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("service")) {
            insideService = false;

            if (!currentServiceType.isEmpty() && !currentControlUrl.isEmpty())
                controlUrls.insert(currentServiceType, currentControlUrl);
        }
    }

    if (reader.hasError())
        return {};

    const QUrl base = declaredBase.isValid() && !declaredBase.host().isEmpty()
                          ? declaredBase
                          : baseUrl;

    // Preference order rather than first-found. A router may advertise several, and
    // taking whichever happened to appear first in the document would be arbitrary.
    for (const char* wanted : WantedServices) {
        const QString type = QString::fromLatin1(wanted);

        const auto found = controlUrls.constFind(type);
        if (found == controlUrls.constEnd())
            continue;

        return { base.resolved(QUrl(*found)), type };
    }

    return {};
}

QString parseSoapValue(const QByteArray& xml, const QString& elementName)
{
    QXmlStreamReader reader(xml);

    while (!reader.atEnd()) {
        reader.readNext();

        // Local name only. The namespace prefix a router picks for the response body
        // varies, and matching on the qualified name would work until one of them
        // chose differently.
        if (reader.isStartElement() && reader.name() == elementName)
            return reader.readElementText().trimmed();
    }

    return {};
}

QString parseSoapFault(const QByteArray& xml)
{
    const QString code        = parseSoapValue(xml, QStringLiteral("errorCode"));
    const QString description = parseSoapValue(xml, QStringLiteral("errorDescription"));

    if (code.isEmpty() && description.isEmpty())
        return {};

    // Both, because neither is sufficient alone: the description is terse to the
    // point of being unhelpful ("ConflictInMappingEntry"), and the code is what can
    // be looked up.
    if (code.isEmpty())
        return description;

    return description.isEmpty()
               ? QStringLiteral("UPnP error %1").arg(code)
               : QStringLiteral("UPnP error %1: %2").arg(code, description);
}

} // namespace upnp

// ── Client ──────────────────────────────────────────────────────────────────

UpnpClient::UpnpClient(QObject* parent)
    : QObject(parent)
    , m_net(std::make_unique<QNetworkAccessManager>())
    , m_discoveryAddress(SsdpGroup)
{
}

UpnpClient::~UpnpClient() = default;

void UpnpClient::setDiscoveryEndpoint(const QHostAddress& address, quint16 port)
{
    m_discoveryAddress = address;
    m_discoveryPort    = port;
}

void UpnpClient::fail(const QString& reason)
{
    m_lastError = reason;
    emit failed(reason);
}

void UpnpClient::discover()
{
    m_service = {};
    m_lastError.clear();

    m_ssdp = std::make_unique<QUdpSocket>();

    if (!m_ssdp->bind(QHostAddress::AnyIPv4, 0)) {
        m_lastError = m_ssdp->errorString();
        emit discoveryFinished(false);
        return;
    }

    // First usable answer wins, and the rest are ignored. A home network has one
    // gateway; where it answers twice — once per device type searched — the replies
    // describe the same router.
    connect(m_ssdp.get(), &QUdpSocket::readyRead, this, [this]() {
        while (m_ssdp && m_ssdp->hasPendingDatagrams()) {
            QByteArray datagram(int(m_ssdp->pendingDatagramSize()), Qt::Uninitialized);
            m_ssdp->readDatagram(datagram.data(), datagram.size());

            const QUrl location = upnp::parseSsdpLocation(datagram);
            if (!location.isValid())
                continue;

            // Stop listening before fetching, so later replies cannot start a second
            // fetch while this one is in flight.
            m_ssdp.reset();
            fetchDeviceDescription(location);
            return;
        }
    });

    for (const char* deviceType : { "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                    "urn:schemas-upnp-org:device:InternetGatewayDevice:1" }) {
        m_ssdp->writeDatagram(searchRequest(deviceType),
                              m_discoveryAddress,
                              m_discoveryPort);
    }

    QTimer::singleShot(DiscoveryTimeoutMs, this, [this]() {
        if (!m_ssdp)
            return;   // Already answered and moved on to the description fetch.

        m_ssdp.reset();
        m_lastError = QStringLiteral("No UPnP gateway answered. It is usually "
                                     "switched off in the router's settings.");
        emit discoveryFinished(false);
    });
}

void UpnpClient::fetchDeviceDescription(const QUrl& location)
{
    QNetworkRequest request(location);
    request.setTransferTimeout(RequestTimeoutMs);

    QNetworkReply* reply = m_net->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, location]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            m_lastError = reply->errorString();
            emit discoveryFinished(false);
            return;
        }

        m_service = upnp::parseDeviceDescription(reply->readAll(), location);

        if (!m_service.isValid()) {
            m_lastError = QStringLiteral("The router answered but offers no "
                                         "port-mapping service.");
        }

        emit discoveryFinished(m_service.isValid());
    });
}

void UpnpClient::callSoap(const QString&                         action,
                          const QString&                         body,
                          std::function<void(const QByteArray&)> onReply)
{
    if (!m_service.isValid()) {
        fail(QStringLiteral("No UPnP gateway has been found."));
        return;
    }

    const QString soapAction =
        QStringLiteral("\"%1#%2\"").arg(m_service.serviceType, action);

    const QByteArray envelope =
        QStringLiteral("<?xml version=\"1.0\"?>"
                       "<s:Envelope "
                       "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                       "<s:Body><u:%1 xmlns:u=\"%2\">%3</u:%1></s:Body>"
                       "</s:Envelope>")
            .arg(action, m_service.serviceType, body)
            .toUtf8();

    QNetworkRequest request(m_service.controlUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("text/xml; charset=\"utf-8\""));
    request.setRawHeader(QByteArrayLiteral("SOAPAction"), soapAction.toUtf8());
    request.setTransferTimeout(RequestTimeoutMs);

    QNetworkReply* reply = m_net->post(request, envelope);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, onReply = std::move(onReply)]() {
        reply->deleteLater();

        const QByteArray payload = reply->readAll();

        // The fault is read before the transport error is considered, because a UPnP
        // refusal arrives as HTTP 500 with the real reason in the body. Reporting
        // "Internal Server Error" instead of "error 718: conflicts with an existing
        // mapping" would send the owner looking in entirely the wrong place.
        const QString fault = upnp::parseSoapFault(payload);
        if (!fault.isEmpty()) {
            fail(fault);
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            fail(reply->errorString());
            return;
        }

        onReply(payload);
    });
}

void UpnpClient::requestExternalAddress()
{
    callSoap(QStringLiteral("GetExternalIPAddress"), QString(),
             [this](const QByteArray& payload) {
        const QString address =
            upnp::parseSoapValue(payload, QStringLiteral("NewExternalIPAddress"));

        if (address.isEmpty()) {
            fail(QStringLiteral("The router did not report an external address."));
            return;
        }

        emit externalAddressReceived(address);
    });
}

void UpnpClient::requestPortMapping(quint16        externalPort,
                                    quint16        internalPort,
                                    const QString& internalClient,
                                    const QString& description)
{
    // NewRemoteHost empty means "from anywhere", which is the point of a server.
    // NewLeaseDuration zero means no expiry; a leased mapping would quietly lapse
    // while the server was still running.
    const QString body =
        QStringLiteral("<NewRemoteHost></NewRemoteHost>"
                       "<NewExternalPort>%1</NewExternalPort>"
                       "<NewProtocol>TCP</NewProtocol>"
                       "<NewInternalPort>%2</NewInternalPort>"
                       "<NewInternalClient>%3</NewInternalClient>"
                       "<NewEnabled>1</NewEnabled>"
                       "<NewPortMappingDescription>%4</NewPortMappingDescription>"
                       "<NewLeaseDuration>%5</NewLeaseDuration>")
            .arg(QString::number(externalPort),
                 QString::number(internalPort),
                 internalClient,
                 description,
                 QString::number(PermanentLease));

    callSoap(QStringLiteral("AddPortMapping"), body,
             [this](const QByteArray&) { emit portMappingFinished(true); });
}

} // namespace core
