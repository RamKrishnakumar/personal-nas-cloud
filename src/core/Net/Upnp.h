#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QUdpSocket;

namespace core {

/**
 * @brief Where a router's port-mapping service lives, and what to call it.
 *
 * The service type is carried alongside the URL because it is part of every
 * subsequent request: SOAP actions are namespaced by it, and a router offering
 * WANPPPConnection must be addressed as such rather than as WANIPConnection. Getting
 * that wrong produces a 401 or an empty fault, which looks like a permissions problem
 * and is not.
 */
struct UpnpService
{
    QUrl    controlUrl;
    QString serviceType;

    bool isValid() const { return controlUrl.isValid() && !serviceType.isEmpty(); }
};

/**
 * @brief Pure parsing for the three replies UPnP involves.
 *
 * Separated from the socket work for the same reason http::RangeParser and
 * http::RequestParser are: these are where the fiddly real-world variation lives —
 * header case, relative URLs, nested device trees, namespace prefixes — and all of it
 * is testable from a QByteArray without a router on the network. The client below is
 * then thin enough to be mostly uninteresting.
 */
namespace upnp {

/**
 * @brief Extracts the LOCATION header from an SSDP reply.
 *
 * SSDP looks like HTTP but is not, and header case is not guaranteed: routers in the
 * wild send `LOCATION`, `Location` and `location`. Matched case-insensitively for
 * that reason.
 *
 * @return The URL, or empty when the reply carries no usable location.
 */
QUrl parseSsdpLocation(const QByteArray& reply);

/**
 * @brief Finds the port-mapping service in a device description document.
 *
 * The service is buried two device levels down — InternetGatewayDevice contains a
 * WANDevice which contains a WANConnectionDevice — so this walks every `<service>`
 * in the document rather than trying to follow that structure. Preference order is
 * WANIPConnection:2, then :1, then WANPPPConnection:1, which is newest-first;
 * PPP is what DSL routers offer and is the one this household's connection uses.
 *
 * @param baseUrl The URL the document was fetched from. Control URLs are almost
 *        always relative, and a document may also carry a `<URLBase>` that overrides
 *        this for resolving them — which is handled here, because ignoring it is the
 *        classic way UPnP works against one router brand and not another.
 */
UpnpService parseDeviceDescription(const QByteArray& xml, const QUrl& baseUrl);

/**
 * @brief Reads one element's text out of a SOAP response body.
 *
 * Matches on the local name, ignoring whatever namespace prefix the router chose —
 * they vary, and binding to one would work until it did not.
 *
 * @return The text, or empty when the element is absent.
 */
QString parseSoapValue(const QByteArray& xml, const QString& elementName);

/**
 * @brief Reads a SOAP fault description, for when a call is refused.
 *
 * UPnP faults carry a numeric `errorCode` and a terse `errorDescription`. Both are
 * worth surfacing: the common refusals when adding a port mapping are 718
 * (conflicts with an existing mapping) and 606 (the router will not accept mappings
 * from this client), and they need completely different advice.
 *
 * @return A description including the code, or empty when there is no fault.
 */
QString parseSoapFault(const QByteArray& xml);

} // namespace upnp

/**
 * @class UpnpClient
 * @brief Asks the router about itself, and optionally opens a port on it.
 *
 * ### What this is for
 *
 * Two questions, both of which the owner otherwise has to answer by hand.
 *
 * The first is *what address does the router think it has*. Compared against what the
 * internet reports, that settles whether the connection is behind the provider's own
 * NAT — and it settles it properly, rather than by guessing from address ranges. The
 * guess is not good enough: the connection this was developed on reports `172.40.x`,
 * which is neither private space nor the range RFC 6598 reserves for the purpose, so
 * a range check calls it public when it is not.
 *
 * The second is *please forward this port*, which removes the hardest step in the
 * whole setup for anybody who has a public address.
 *
 * ### Asynchronous, deliberately
 *
 * Discovery waits a couple of seconds for routers to answer. Doing that synchronously
 * at startup would freeze the window for exactly as long, which is the sort of stall
 * the whole of §5 of the v0.6 plan was about removing. So this emits signals and
 * nothing here ever blocks, following DdnsUpdater.
 *
 * ### When UPnP is simply not there
 *
 * Plenty of routers ship with it disabled, and some providers lock it off entirely.
 * That is an ordinary outcome, not a fault: discovery reports failure, and the owner
 * is asked to make the one comparison by hand instead.
 */
class UpnpClient : public QObject
{
    Q_OBJECT

public:
    /** How long to wait for routers to answer an SSDP search. */
    static constexpr int DiscoveryTimeoutMs = 3'000;

    /** How long any one SOAP call may take. */
    static constexpr int RequestTimeoutMs = 8'000;

    /** A port mapping with no expiry, which is what a server wants. */
    static constexpr int PermanentLease = 0;

    explicit UpnpClient(QObject* parent = nullptr);
    ~UpnpClient() override;

    /**
     * @brief Overrides where the SSDP search is sent.
     *
     * Exists so a test can point discovery at a responder on localhost. Multicast to
     * the real group cannot be answered in a unit test, and leaving the only I/O path
     * in this class untestable to save one setter is a poor trade.
     */
    void setDiscoveryEndpoint(const QHostAddress& address, quint16 port);

    /** The service found by discovery, or an invalid one. */
    UpnpService service() const { return m_service; }
    bool        isAvailable() const { return m_service.isValid(); }

    /** Reason the last operation failed, or empty. */
    QString lastError() const { return m_lastError; }

    /** Searches for a gateway. Emits discoveryFinished() either way. */
    void discover();

    /**
     * @brief Asks the router for the address it believes it has on the internet.
     *
     * Requires a successful discover(). Emits externalAddressReceived() or failed().
     */
    void requestExternalAddress();

    /**
     * @brief Asks the router to forward @p externalPort to this machine.
     *
     * Emits portMappingFinished(). A mapping already pointing here counts as
     * success — re-running this should be safe, and routers differ on whether they
     * replace an identical mapping or refuse it with fault 718.
     *
     * @param internalClient This machine's LAN address; FileServer::localIP()
     *        supplies it.
     */
    void requestPortMapping(quint16       externalPort,
                            quint16       internalPort,
                            const QString& internalClient,
                            const QString& description);

signals:
    /** @param found True when a usable gateway service was located. */
    void discoveryFinished(bool found);

    void externalAddressReceived(QString address);
    void portMappingFinished(bool succeeded);

    /** Any failure after discovery. Carries a reason fit to show the owner. */
    void failed(QString reason);

private:
    /** Issues one SOAP action; @p onReply receives the body. */
    void callSoap(const QString&                           action,
                  const QString&                           body,
                  std::function<void(const QByteArray&)>   onReply);

    void fetchDeviceDescription(const QUrl& location);
    void fail(const QString& reason);

    std::unique_ptr<QNetworkAccessManager> m_net;
    std::unique_ptr<QUdpSocket>            m_ssdp;

    UpnpService m_service;
    QString     m_lastError;

    QHostAddress m_discoveryAddress;
    quint16      m_discoveryPort = 1900;
};

} // namespace core
