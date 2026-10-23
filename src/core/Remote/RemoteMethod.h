#pragma once

#include <QString>

#include "core/Net/ConnectionCheck.h"

namespace core {

/**
 * @brief How the library is reached from outside the house.
 *
 * ### Why this is a choice rather than both at once
 *
 * The two paths need certificates for different names — a `.ts.net` MagicDNS name for
 * one, the DDNS hostname for the other — and Qt 6 exposes no server-side SNI hook, so
 * one listener can present exactly one certificate. Serving both with a certificate
 * browsers trust would need two listeners with two worker pools, which is a great
 * deal of machinery for a household with one connection in use at a time.
 *
 * So it is one or the other, and the right one depends on the connection.
 */
enum class RemoteMethod
{
    /** Work it out from the connection. The default. */
    Auto,

    /**
     * Through the private overlay network. Works on any connection, including behind
     * the provider's NAT, and needs no router configuration — but every viewing
     * device must have Tailscale installed and signed in.
     */
    Tailscale,

    /**
     * Through a forwarded port and the DDNS hostname. Needs a public address and a
     * rule on the router, and in exchange gives an ordinary link that anybody can
     * open with nothing installed.
     */
    Public,

    /** Nothing available; the local network only. */
    LanOnly,
};

QString describeRemoteMethod(RemoteMethod method);

/** For config.ini, which has to round-trip the setting. */
QString      remoteMethodToString(RemoteMethod method);
RemoteMethod remoteMethodFromString(const QString& text);

/**
 * @brief Picks the method to actually use.
 *
 * Pure, so every combination is reachable in a test. There are more of them than
 * there look to be, and the awkward ones are the cases where what was configured is
 * not available — a public link asked for on a connection that cannot carry one, or
 * Tailscale asked for while signed out. Falling back silently would leave the owner
 * with no remote access and no explanation, so the caller is expected to report the
 * difference between @p configured and the result.
 *
 * Auto prefers Public when it is available, because that path needs nothing installed
 * on the viewing device. Where it is not, Tailscale works everywhere else.
 *
 * @param configured What the owner chose, or Auto.
 * @param connection The detected connection type. Unknown is treated as not
 *        confirmed public: guessing wrong towards Public yields a link that times
 *        out, which is a worse first experience than a link that needs an app.
 * @param tailscaleOnline Whether the node is present and signed in.
 * @param hasPublicHostname Whether a DDNS hostname is configured. Without one the
 *        public path could only offer a bare address, which no certificate will
 *        match.
 */
RemoteMethod resolveRemoteMethod(RemoteMethod   configured,
                                 ConnectionType connection,
                                 bool           tailscaleOnline,
                                 bool           hasPublicHostname);

} // namespace core
