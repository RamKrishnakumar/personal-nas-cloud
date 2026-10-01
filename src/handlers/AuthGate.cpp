#include "handlers/AuthGate.h"

#include "auth/PermissionStore.h"
#include "auth/Permissions.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/Connection.h"
#include "http/Cookies.h"
#include "http/RequestParser.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {

/** True when the client is a browser navigating, rather than script fetching. */
bool wantsHtml(const http::Request& request)
{
    // A fetch() from the SPA sends Accept: application/json or */*, while a
    // navigation sends text/html first. Only a navigation benefits from a redirect;
    // redirecting an XHR produces an HTML parse error instead of a clear signal.
    if (request.method != "GET" && request.method != "HEAD")
        return false;

    const QByteArray accept = request.header("accept").toByteArray();
    if (!accept.contains("text/html"))
        return false;

    // Sec-Fetch-Mode is sent by every current browser and says outright whether
    // this is a navigation. When present it is more reliable than Accept.
    const QByteArray mode = request.header("sec-fetch-mode").toByteArray();
    if (!mode.isEmpty())
        return mode == "navigate";

    return true;
}

} // namespace

namespace handlers {

AuthGate::AuthGate(FileRouter*            inner,
                   AuthHandler*           authentication,
                   auth::SessionStore*    sessions,
                   auth::UserStore*       users,
                   auth::PermissionStore* permissions)
    : m_inner(inner)
    , m_authentication(authentication)
    , m_sessions(sessions)
    , m_users(users)
    , m_permissions(permissions)
{
}

bool AuthGate::isPublicPath(const QByteArray& path)
{
    if (AuthHandler::handles(path))
        return true;

    // Script and stylesheet only. These carry no file data, and the login page
    // would be unstyled without them.
    if (path.startsWith(QByteArrayLiteral("/_static/")))
        return true;

    return path == QByteArrayLiteral("/healthz");
}

void AuthGate::handle(http::Connection& connection, const http::Request& request)
{
    // ── Liveness ──────────────────────────────────────────────────────────
    // Answered here rather than in the file router: it must work before anyone has
    // signed in, and it must reveal nothing about what is stored.
    if (request.path == QByteArrayLiteral("/healthz")) {
        connection.send(200, "text/plain; charset=utf-8", "ok\n",
                        "Cache-Control: no-store\r\n");
        return;
    }

    if (m_authentication && AuthHandler::handles(request.path)) {
        m_authentication->handle(connection, request);
        return;
    }

    if (request.path.startsWith(QByteArrayLiteral("/_static/"))) {
        m_inner->handle(connection, request, auth::AuthContext{});
        return;
    }

    // ── Everything else needs a session ───────────────────────────────────
    if (!m_sessions || !m_users) {
        // Refused rather than served. Reaching here means the gate was installed
        // without its stores, and guessing the permissive answer would expose the
        // whole library.
        connection.sendError(503);
        return;
    }

    const QByteArray token = http::cookieValue(request, SessionCookieName);
    if (token.isEmpty()) {
        sendAuthenticationRequired(connection, request);
        return;
    }

    const auto session = m_sessions->validate(token);
    if (!session) {
        sendAuthenticationRequired(connection, request);
        return;
    }

    const auto user = m_users->findById(session->userId);
    if (!user || !user->enabled) {
        // The account was deleted or disabled while the session was still valid.
        // Ending the session here is what makes disabling an account take effect
        // immediately rather than at its next expiry.
        m_sessions->destroy(token);
        sendAuthenticationRequired(connection, request);
        return;
    }

    auth::AuthContext context;
    context.user          = *user;
    context.authenticated = true;
    if (m_permissions)
        context.folderPerms = m_permissions->forUser(user->id);

    m_inner->handle(connection, request, context);
}

void AuthGate::sendAuthenticationRequired(http::Connection&    connection,
                                          const http::Request& request)
{
    const QByteArray clearCookie =
        QByteArray("Set-Cookie: ") + SessionCookieName
        + "=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0\r\n";

    if (wantsHtml(request)) {
        // The original target is carried through so signing in lands the person
        // where they were going, not on the root.
        QByteArray location = "/login";
        if (!request.target.isEmpty() && request.target != QByteArrayLiteral("/")) {
            location += "?next=" + QUrl::toPercentEncoding(
                            QString::fromUtf8(request.target));
        }

        QByteArray headers = "Location: " + location + "\r\n";
        headers += clearCookie;
        headers += "Cache-Control: no-store\r\n";

        // 302 rather than 301: this is a temporary condition, and a browser that
        // cached a permanent redirect would keep sending signed-in users to the
        // login page.
        connection.send(302, "text/html; charset=utf-8",
                        "<!doctype html><meta charset=\"utf-8\">"
                        "<title>Sign in</title>"
                        "<p>Please <a href=\"/login\">sign in</a>.",
                        headers);
        return;
    }

    QJsonObject body;
    body[QStringLiteral("error")]         = QStringLiteral("Authentication required.");
    body[QStringLiteral("authenticated")] = false;
    body[QStringLiteral("login")]         = QStringLiteral("/login");

    QByteArray headers = clearCookie;
    headers += "Cache-Control: no-store\r\n";

    connection.send(401, "application/json; charset=utf-8",
                    QJsonDocument(body).toJson(QJsonDocument::Compact), headers);
}

} // namespace handlers
