#include "core/Remote/RemoteMethod.h"

#include <QObject>

namespace core {

QString describeRemoteMethod(RemoteMethod method)
{
    switch (method) {
    case RemoteMethod::Auto:
        return QObject::tr("Automatic");
    case RemoteMethod::Tailscale:
        return QObject::tr("Tailscale");
    case RemoteMethod::Public:
        return QObject::tr("Public link");
    case RemoteMethod::LanOnly:
        break;
    }

    return QObject::tr("This network only");
}

QString remoteMethodToString(RemoteMethod method)
{
    switch (method) {
    case RemoteMethod::Tailscale:
        return QStringLiteral("tailscale");
    case RemoteMethod::Public:
        return QStringLiteral("public");
    case RemoteMethod::LanOnly:
        return QStringLiteral("lan");
    case RemoteMethod::Auto:
        break;
    }

    return QStringLiteral("auto");
}

RemoteMethod remoteMethodFromString(const QString& text)
{
    const QString value = text.trimmed().toLower();

    if (value == QLatin1String("tailscale"))
        return RemoteMethod::Tailscale;

    if (value == QLatin1String("public"))
        return RemoteMethod::Public;

    if (value == QLatin1String("lan"))
        return RemoteMethod::LanOnly;

    return RemoteMethod::Auto;
}

RemoteMethod resolveRemoteMethod(RemoteMethod   configured,
                                 ConnectionType connection,
                                 bool           tailscaleOnline,
                                 bool           hasPublicHostname)
{
    // Only a confirmed public address counts. Unknown means the router would not say
    // and the address was not in a range that settles it, and choosing the public
    // path on that basis produces a link that times out with no explanation — worse
    // than a link that asks for an app to be installed.
    const bool publicPossible =
        connection == ConnectionType::PublicIp && hasPublicHostname;

    switch (configured) {
    case RemoteMethod::LanOnly:
        return RemoteMethod::LanOnly;

    case RemoteMethod::Public:
        // Honoured only when it can actually work. An explicit choice is respected
        // where possible, but a rule on a router behind the provider's NAT has
        // nothing on the internet able to match it, so there is nothing to honour —
        // fall back rather than pretend.
        if (publicPossible)
            return RemoteMethod::Public;

        return tailscaleOnline ? RemoteMethod::Tailscale : RemoteMethod::LanOnly;

    case RemoteMethod::Tailscale:
        return tailscaleOnline ? RemoteMethod::Tailscale : RemoteMethod::LanOnly;

    case RemoteMethod::Auto:
        break;
    }

    // Public first: it is the only one of the two that lets somebody open the link
    // without installing anything, which is worth more than the convenience of not
    // configuring a router.
    if (publicPossible)
        return RemoteMethod::Public;

    if (tailscaleOnline)
        return RemoteMethod::Tailscale;

    return RemoteMethod::LanOnly;
}

} // namespace core
