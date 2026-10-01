#pragma once

#include <QByteArray>

#include "http/Router.h"

namespace auth {
class PermissionStore;
class SessionStore;
class UserStore;
}

namespace handlers {

class AuthHandler;
class FileRouter;

/**
 * @class AuthGate
 * @brief The single point where a request becomes authenticated, or does not.
 *
 * A decorator rather than a change to every handler, which is what `http::Router`
 * was separated from `Connection` for. `HttpServer` is given this; this is given
 * the FileRouter.
 *
 * ### What it does and deliberately does not do
 *
 * It establishes **who** is asking and refuses anonymous requests. It does not
 * decide **what** they may see, because it cannot: `/api/list?path=/Private` and
 * `/_thumb/s/Private/x.jpg` both name a target the gate would have to re-parse to
 * know, and a second parser that disagreed with the handler's would be a
 * confused-deputy bug. Authorisation therefore happens inside FileRouter, against
 * the path it has already resolved, using the `auth::AuthContext` this gate
 * produces.
 *
 * ### Public routes
 *
 * Only these, and nothing that reads the library:
 *   - `/login`, `/api/login`, `/api/logout`, `/api/session` — the way in
 *   - `/_static/*` — the stylesheet and script, no file data
 *   - `/healthz` — liveness, no information about contents
 *
 * ### Answering an anonymous request
 *
 * A browser navigating gets `302 /login?next=<where they were going>`, so signing
 * in returns them to the page they asked for. Anything else — an API call, a
 * fetch, an image — gets `401` with a JSON body, because redirecting an XHR to an
 * HTML page produces a confusing parse error rather than a usable signal.
 *
 * Thread-safe: every worker calls handle() concurrently, and the stores it uses
 * serialise themselves.
 */
class AuthGate : public http::Router
{
public:
    AuthGate(FileRouter*            inner,
             AuthHandler*           authentication,
             auth::SessionStore*    sessions,
             auth::UserStore*       users,
             auth::PermissionStore* permissions);

    void handle(http::Connection& connection, const http::Request& request) override;

    /** True for a path served without a session. */
    static bool isPublicPath(const QByteArray& path);

private:
    void sendAuthenticationRequired(http::Connection&    connection,
                                    const http::Request& request);

    FileRouter*            m_inner          = nullptr;
    AuthHandler*           m_authentication = nullptr;
    auth::SessionStore*    m_sessions       = nullptr;
    auth::UserStore*       m_users          = nullptr;
    auth::PermissionStore* m_permissions    = nullptr;
};

} // namespace handlers
