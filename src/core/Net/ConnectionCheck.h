#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QNetworkAccessManager;

namespace core {

class UpnpClient;

/**
 * @brief What kind of internet connection this machine is on.
 *
 * The distinction that decides how the library can be reached from outside, and the
 * one the owner otherwise has to work out by reading two numbers off two different
 * screens.
 */
enum class ConnectionType
{
    /**
     * Could not be determined. The honest answer when the router will not say what
     * address it has — usually because UPnP is switched off — and the public address
     * is not in a range that settles it either way.
     */
    Unknown,

    /**
     * The router holds the address the internet sees, so forwarding a port on it
     * works and an ordinary shareable link is possible.
     */
    PublicIp,

    /**
     * The provider operates a NAT in front of the router. Forwarding a port cannot
     * work: nothing on the internet can address the router, so a rule on it has
     * nothing to match. This is not a misconfiguration and there is no setting that
     * changes it.
     */
    CarrierNat,
};

/** Short, fit to show the owner. */
QString describeConnectionType(ConnectionType type);

/** For the config file, which has to round-trip the cached answer. */
QString  connectionTypeToString(ConnectionType type);
ConnectionType connectionTypeFromString(const QString& text);

/**
 * @brief Decides the connection type from the two addresses.
 *
 * The whole decision, as a pure function, so every branch is reachable in a test
 * without a network or a router.
 *
 * Comparing the two addresses is the reliable test, and it is reliable because it
 * asks the question directly. Range checks do not: the connection this was developed
 * on reports a router address of `172.40.42.138`, which is neither RFC 1918 private
 * space nor the `100.64.0.0/10` that RFC 6598 reserves for carrier-grade NAT — the
 * provider is using squat space. A range check calls that public. It is not; the
 * internet sees `111.91.226.113`.
 *
 * @param routerWanIp What the router says its own internet-facing address is, from
 *        UPnP. Empty when UPnP could not answer.
 * @param publicIp What an external service reports seeing. Empty when unreachable.
 */
ConnectionType classifyConnection(const QString& routerWanIp,
                                  const QString& publicIp);

/**
 * @class ConnectionCheck
 * @brief Finds out which kind of connection this is, asynchronously.
 *
 * Asks the router over UPnP and an external service over HTTPS, then compares. Both
 * are network round trips with timeouts measured in seconds, so this reports through
 * a signal rather than blocking — a synchronous version would stall the window at
 * every startup for as long as the slower of the two takes.
 *
 * A result of Unknown is a real answer, not a failure. It means the owner has to make
 * the comparison by hand once, which takes half a minute, and the answer can then be
 * remembered in the configuration rather than being asked again at every launch.
 */
class ConnectionCheck : public QObject
{
    Q_OBJECT

public:
    /**
     * @param upnp Used to ask the router. May be null, in which case only the public
     *        address is fetched and the verdict rests on the range check alone.
     *        Not owned; must outlive this object.
     */
    explicit ConnectionCheck(UpnpClient* upnp, QObject* parent = nullptr);
    ~ConnectionCheck() override;

    /** Starts the check. Emits finished() exactly once. */
    void start();

    ConnectionType result() const { return m_result; }
    QString routerWanIp() const { return m_routerWanIp; }
    QString publicIp() const { return m_publicIp; }

    /** The evidence, as a sentence or two fit to show the owner. */
    QString explanation() const;

signals:
    void finished(ConnectionType type);

private:
    void fetchPublicAddress();
    void conclude();

    UpnpClient* m_upnp = nullptr;
    std::unique_ptr<QNetworkAccessManager> m_net;

    QString m_routerWanIp;
    QString m_publicIp;

    ConnectionType m_result = ConnectionType::Unknown;

    bool m_routerDone = false;
    bool m_publicDone = false;
};

/**
 * @brief The service used to discover the public address.
 *
 * Declared here and used by DdnsUpdater as well, so the two cannot drift onto
 * different services and start disagreeing about what the public address is — which
 * would make this check's comparison meaningless in a way that would be very hard
 * to see.
 */
extern const char* const PublicAddressService;

} // namespace core
