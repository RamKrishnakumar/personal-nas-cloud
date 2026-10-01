#pragma once

#include <QAtomicInt>
#include <QAtomicInteger>
#include <QByteArray>
#include <QObject>
#include <QString>

namespace auth {
class LoginRateLimiter;
class SessionStore;
class UserStore;
}

namespace http {
class Connection;
struct Request;
}

class QThreadPool;

namespace handlers {

/** Cookie the session token travels in. */
inline constexpr char SessionCookieName[] = "nd_session";

/**
 * @class AuthHandler
 * @brief Serves the login page and the login, logout and session endpoints.
 *
 * ### Password verification never runs on a worker thread
 *
 * PBKDF2 at the configured work factor costs roughly 1.4 s on a modern laptop and
 * an estimated 3–4 s on the reference i5 6th gen. The Light hardware tier runs two
 * worker threads, so verifying inline would stop the server serving anything for
 * seconds at a time — on a machine whose whole point is staying responsive.
 *
 * Verification therefore happens on a dedicated single-thread executor and the
 * response is deferred. One thread, not a pool, is also the answer to the CPU
 * exhaustion this work factor invites: however many login attempts arrive, at most
 * one core is ever busy hashing.
 *
 * Attempts beyond MaxQueuedLogins are refused with 503 rather than queued, because
 * a queue deeper than that would outlive Connection's deferral timeout and the
 * client would be answered 504 after waiting for nothing.
 *
 * ### Timing does not reveal which usernames exist
 *
 * An unknown or disabled account is verified against auth::decoyHash() instead of
 * being rejected early, so every rejected attempt costs one full derivation and
 * returns the same message. Without this, a stopwatch enumerates the account list.
 *
 * Thread-safe: every worker thread calls handle() concurrently.
 */
class AuthHandler : public QObject
{
    Q_OBJECT

public:
    /** Concurrent verifications allowed to wait. See the class documentation. */
    static constexpr int MaxQueuedLogins = 2;

    AuthHandler(auth::UserStore*        users,
                auth::SessionStore*     sessions,
                auth::LoginRateLimiter* limiter,
                QObject*                parent = nullptr);
    ~AuthHandler() override;

    /** True when @p path is one this handler owns, in any accepted spelling. */
    static bool handles(const QByteArray& path);

    /**
     * @brief The canonical spelling of an auth path, or empty if it is not one.
     *
     * A trailing slash and any capitalisation are accepted and redirected to the
     * one true form. Both are easy to produce by accident — a phone keyboard
     * autocapitalises the first letter, so a hand-typed address arrives as
     * `/Login` — and answering 404 to someone trying to reach the sign-in page is
     * indistinguishable from the feature being missing.
     *
     * Deliberately limited to this fixed set of endpoints. The rest of the router
     * stays case-sensitive, because those paths are filenames and folding them
     * would be a correctness bug rather than a kindness.
     */
    static QByteArray canonicalPath(const QByteArray& path);

    /**
     * @brief Serves @p request. Only call when handles() returned true.
     *
     * Responds on @p connection exactly once, possibly after deferring.
     */
    void handle(http::Connection& connection, const http::Request& request);

signals:
    /**
     * @brief Carries a finished verification back to the requesting thread.
     *
     * Connected with the Connection as receiver, so Qt both delivers it in that
     * connection's thread and drops the delivery if the client went away while the
     * hash was being computed.
     */
    void loginResolved(quint64 requestId, bool accepted, int userId);

private:
    void handleLogin(http::Connection& connection, const http::Request& request);
    void handleLogout(http::Connection& connection, const http::Request& request);
    void handleSession(http::Connection& connection, const http::Request& request);
    void sendLoginPage(http::Connection& connection);

    /** Runs on the hashing thread. */
    void verifyCredentials(quint64        requestId,
                           const QString& username,
                           const QString& password);

    /** Issues the cookie and the success body, or answers 401. */
    void completeLogin(http::Connection& connection,
                       const http::Request& request,
                       bool           accepted,
                       int            userId,
                       const QString& username,
                       bool           rememberMe);

    auth::UserStore*        m_users    = nullptr;
    auth::SessionStore*     m_sessions = nullptr;
    auth::LoginRateLimiter* m_limiter  = nullptr;

    QThreadPool* m_hasher = nullptr;   ///< Exactly one thread.

    QAtomicInteger<quint64> m_nextRequestId = 1;
    QAtomicInt              m_queuedLogins  = 0;
};

} // namespace handlers
