#include "handlers/AuthHandler.h"

#include "auth/LoginRateLimiter.h"
#include "auth/PasswordHash.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "http/Connection.h"
#include "http/Cookies.h"
#include "http/RequestParser.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QThreadPool>

#include <memory>
#include <optional>

namespace {

/**
 * One message for every rejection.
 *
 * Saying "no such user" for one case and "wrong password" for the other hands an
 * attacker the account list, which is why §8.3 of the plan requires both the text
 * and the time taken to be identical.
 */
const char RejectionMessage[] = "Incorrect username or password.";

QByteArray jsonBody(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void sendJson(http::Connection& connection, int statusCode,
              const QJsonObject& object, const QByteArray& extraHeaders = {})
{
    connection.send(statusCode, "application/json; charset=utf-8",
                    jsonBody(object), extraHeaders);
}

void sendJsonError(http::Connection& connection, int statusCode,
                   const QString& message, const QByteArray& extraHeaders = {})
{
    QJsonObject object;
    object[QStringLiteral("error")] = message;
    sendJson(connection, statusCode, object, extraHeaders);
}

/**
 * @brief Builds the Set-Cookie header for a session.
 *
 * HttpOnly keeps the token out of reach of scripts, so a stored-XSS bug cannot be
 * escalated into account takeover. SameSite=Lax stops a third-party page from
 * silently issuing authenticated requests while still allowing ordinary top-level
 * navigation into NestDrive. Secure is conditional: setting it over plain HTTP
 * would make the cookie unusable on a LAN deployment with no certificate.
 */
QByteArray sessionCookieHeader(const QByteArray& token, qint64 maxAgeSeconds,
                               bool secure)
{
    QByteArray header = "Set-Cookie: ";
    header += handlers::SessionCookieName;
    header += '=';
    header += token;
    header += "; HttpOnly; SameSite=Lax; Path=/";

    if (maxAgeSeconds > 0)
        header += "; Max-Age=" + QByteArray::number(maxAgeSeconds);
    else
        header += "; Max-Age=0";

    if (secure)
        header += "; Secure";

    header += "\r\n";
    return header;
}

} // namespace

namespace handlers {

AuthHandler::AuthHandler(auth::UserStore*        users,
                         auth::SessionStore*     sessions,
                         auth::LoginRateLimiter* limiter,
                         QObject*                parent)
    : QObject(parent)
    , m_users(users)
    , m_sessions(sessions)
    , m_limiter(limiter)
{
    m_hasher = new QThreadPool(this);

    // One thread, deliberately. This bounds the CPU any number of login attempts
    // can consume to a single core, which matters because the work factor that
    // makes the hash strong is also what makes it a denial-of-service lever.
    m_hasher->setMaxThreadCount(1);
    m_hasher->setObjectName(QStringLiteral("nestdrive-auth"));
}

AuthHandler::~AuthHandler()
{
    // Jobs capture `this`, so they must finish before it goes away.
    m_hasher->clear();
    m_hasher->waitForDone();
}

bool AuthHandler::handles(const QByteArray& path)
{
    return path == QByteArrayLiteral("/login")
        || path == QByteArrayLiteral("/api/login")
        || path == QByteArrayLiteral("/api/logout")
        || path == QByteArrayLiteral("/api/session");
}

void AuthHandler::handle(http::Connection& connection, const http::Request& request)
{
    const QByteArray& path = request.path;

    if (path == QByteArrayLiteral("/login")) {
        if (request.method != "GET" && request.method != "HEAD") {
            connection.sendError(405);
            return;
        }
        sendLoginPage(connection);
        return;
    }

    if (path == QByteArrayLiteral("/api/login")) {
        if (request.method != "POST") {
            sendJsonError(connection, 405, QStringLiteral("Use POST to log in."));
            return;
        }
        handleLogin(connection, request);
        return;
    }

    if (path == QByteArrayLiteral("/api/logout")) {
        if (request.method != "POST") {
            sendJsonError(connection, 405, QStringLiteral("Use POST to log out."));
            return;
        }
        handleLogout(connection, request);
        return;
    }

    handleSession(connection, request);
}

void AuthHandler::sendLoginPage(http::Connection& connection)
{
    QFile page(QStringLiteral(":/web/login.html"));
    if (!page.open(QIODevice::ReadOnly)) {
        connection.sendError(500);
        return;
    }

    // no-store rather than a cache lifetime: a cached login page on a shared device
    // is a small thing, but there is no benefit to caching it either.
    connection.send(200, "text/html; charset=utf-8", page.readAll(),
                    "Cache-Control: no-store\r\n");
}

void AuthHandler::handleLogin(http::Connection& connection,
                              const http::Request& request)
{
    if (!m_users || !m_sessions || !m_limiter) {
        sendJsonError(connection, 503,
                      QStringLiteral("Accounts are not configured yet."));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(connection.requestBody(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        sendJsonError(connection, 400, QStringLiteral("Expected a JSON object."));
        return;
    }

    const QJsonObject object   = document.object();
    const QString     username = object.value(QStringLiteral("username")).toString();
    const QString     password = object.value(QStringLiteral("password")).toString();
    const bool        remember = object.value(QStringLiteral("remember")).toBool();

    if (username.isEmpty() || password.isEmpty()) {
        sendJsonError(connection, 400,
                      QStringLiteral("A username and password are required."));
        return;
    }

    const QString address = connection.peerAddress();

    // Checked before any hashing, so a throttled attacker costs no CPU at all.
    const auth::RateLimitDecision decision = m_limiter->check(username, address);
    if (!decision.allowed) {
        const QByteArray retryAfter =
            "Retry-After: " + QByteArray::number(decision.retryAfterSeconds) + "\r\n";
        sendJsonError(connection, 429,
                      QStringLiteral("Too many attempts. Try again later."),
                      retryAfter);
        return;
    }

    // Refused rather than queued: a deeper queue would exceed the connection's
    // deferral timeout, so the client would wait and then be told 504 anyway.
    if (m_queuedLogins.loadAcquire() >= MaxQueuedLogins) {
        sendJsonError(connection, 503,
                      QStringLiteral("The server is busy checking other logins."),
                      "Retry-After: 2\r\n");
        return;
    }

    const quint64 requestId = m_nextRequestId.fetchAndAddOrdered(1);

    // Connected before the job is queued; a fast verification could otherwise
    // finish before anyone was listening. The receiver is the connection, so Qt
    // delivers in its thread and silently drops the result if it died first —
    // which is the only race-free way to handle a client that hung up mid-login.
    auto link = std::make_shared<QMetaObject::Connection>();
    *link = connect(this, &AuthHandler::loginResolved, &connection,
        [this, link, requestId, &connection, request, username, remember](
            quint64 resolvedId, bool accepted, int userId) {
            if (resolvedId != requestId)
                return;
            QObject::disconnect(*link);
            completeLogin(connection, request, accepted, userId, username, remember);
        });

    connection.deferResponse();
    m_queuedLogins.fetchAndAddOrdered(1);

    m_hasher->start([this, requestId, username, password]() {
        verifyCredentials(requestId, username, password);
    });
}

void AuthHandler::verifyCredentials(quint64        requestId,
                                    const QString& username,
                                    const QString& password)
{
    const auto user = m_users->findByUsername(username);

    bool accepted = false;
    int  userId   = 0;

    if (!user || !user->enabled || user->passwordHash.isEmpty()) {
        // Verified against a decoy so this path costs the same as a real one. The
        // result is discarded; only the time it took matters.
        (void)auth::verifyPassword(password, auth::decoyHash());
    } else {
        const auth::VerifyResult result =
            auth::verifyPassword(password, user->passwordHash);

        if (result.ok()) {
            accepted = true;
            userId   = user->id;

            // The one moment the plaintext is available, so the only moment a
            // weaker stored hash can be strengthened.
            if (result.needsUpgrade) {
                const QString upgraded = auth::hashPassword(password);
                if (!upgraded.isEmpty())
                    m_users->updatePasswordHash(user->id, upgraded);
            }
        }
    }

    m_queuedLogins.fetchAndSubOrdered(1);
    emit loginResolved(requestId, accepted, userId);
}

void AuthHandler::completeLogin(http::Connection&    connection,
                                const http::Request& request,
                                bool                 accepted,
                                int                  userId,
                                const QString&       username,
                                bool                 rememberMe)
{
    const QString address = connection.peerAddress();

    if (!accepted) {
        m_limiter->recordFailure(username, address);
        sendJsonError(connection, 401, QString::fromLatin1(RejectionMessage));
        return;
    }

    m_limiter->recordSuccess(username);

    const qint64 idleSeconds = rememberMe ? auth::SessionStore::RememberMeIdleSeconds
                                          : auth::SessionStore::DefaultIdleSeconds;

    const QString userAgent =
        QString::fromUtf8(request.header("user-agent").toByteArray());

    const QByteArray token =
        m_sessions->create(userId, idleSeconds, address, userAgent);

    if (token.isEmpty()) {
        sendJsonError(connection, 500,
                      QStringLiteral("The session could not be created."));
        return;
    }

    m_users->recordLogin(userId);

    const auto user = m_users->findById(userId);

    QJsonObject body;
    body[QStringLiteral("ok")]       = true;
    body[QStringLiteral("username")] = user ? user->username : username;
    body[QStringLiteral("role")]     =
        auth::roleToString(user ? user->role : auth::Role::Guest);

    QByteArray headers =
        sessionCookieHeader(token, idleSeconds, connection.isEncrypted());
    headers += "Cache-Control: no-store\r\n";

    sendJson(connection, 200, body, headers);
}

void AuthHandler::handleLogout(http::Connection& connection,
                               const http::Request& request)
{
    if (m_sessions) {
        const QByteArray token = http::cookieValue(request, SessionCookieName);
        if (!token.isEmpty())
            m_sessions->destroy(token);
    }

    // The cookie is cleared even when there was no session to destroy, so a stale
    // or unknown token does not keep being presented on every later request.
    QByteArray headers = sessionCookieHeader({}, 0, connection.isEncrypted());
    headers += "Cache-Control: no-store\r\n";

    QJsonObject body;
    body[QStringLiteral("ok")] = true;
    sendJson(connection, 200, body, headers);
}

void AuthHandler::handleSession(http::Connection& connection,
                                const http::Request& request)
{
    if (request.method != "GET" && request.method != "HEAD") {
        connection.sendError(405);
        return;
    }

    QJsonObject body;

    const QByteArray token = http::cookieValue(request, SessionCookieName);

    std::optional<auth::Session> session;
    if (m_sessions && !token.isEmpty())
        session = m_sessions->validate(token);

    if (!session) {
        body[QStringLiteral("authenticated")] = false;
        sendJson(connection, 200, body, "Cache-Control: no-store\r\n");
        return;
    }

    const auto user = m_users ? m_users->findById(session->userId) : std::nullopt;
    if (!user || !user->enabled) {
        // The account went away or was disabled while the session lived on.
        if (m_sessions)
            m_sessions->destroy(token);
        body[QStringLiteral("authenticated")] = false;
        sendJson(connection, 200, body, "Cache-Control: no-store\r\n");
        return;
    }

    body[QStringLiteral("authenticated")] = true;
    body[QStringLiteral("username")]      = user->username;
    body[QStringLiteral("role")]          = auth::roleToString(user->role);
    body[QStringLiteral("expiresAt")]     = double(session->expiresAt);

    sendJson(connection, 200, body, "Cache-Control: no-store\r\n");
}

} // namespace handlers
