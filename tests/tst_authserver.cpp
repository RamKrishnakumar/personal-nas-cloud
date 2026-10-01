#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "auth/LoginRateLimiter.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "db/Database.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/Connection.h"
#include "http/Cookies.h"
#include "http/HttpServer.h"

namespace {

struct Response
{
    int        status = 0;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;

    QByteArray header(const QByteArray& lowercaseName) const
    {
        for (const auto& [name, value] : headers)
            if (name == lowercaseName)
                return value;
        return {};
    }

    /** Every Set-Cookie value, since a response may carry more than one. */
    QList<QByteArray> setCookies() const
    {
        QList<QByteArray> values;
        for (const auto& [name, value] : headers)
            if (name == "set-cookie")
                values.append(value);
        return values;
    }

    QJsonObject json() const
    {
        return QJsonDocument::fromJson(body).object();
    }
};

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 30'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

bool readResponse(QTcpSocket* socket, Response* out, int timeoutMs = 30'000)
{
    QByteArray buffer;
    const auto drain = [&] {
        if (socket->bytesAvailable() > 0)
            buffer += socket->readAll();
    };

    if (!spinUntil([&] { drain(); return buffer.contains("\r\n\r\n"); }, timeoutMs))
        return false;

    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');

    const QList<QByteArray> statusParts = lines.first().simplified().split(' ');
    if (statusParts.size() < 2)
        return false;
    out->status = statusParts.at(1).toInt();

    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0) {
            out->headers.append({ line.left(colon).toLower(),
                                  line.mid(colon + 1).trimmed() });
        }
    }

    const qint64 expected = out->header("content-length").toLongLong();
    if (!spinUntil([&] { drain(); return buffer.size() - (headerEnd + 4) >= expected; },
                   timeoutMs)) {
        return false;
    }

    out->body = buffer.mid(headerEnd + 4, expected);
    return true;
}

/** Name=value from a Set-Cookie header, dropping the attributes. */
QByteArray cookiePair(const QByteArray& setCookie)
{
    const qsizetype semicolon = setCookie.indexOf(';');
    return semicolon < 0 ? setCookie : setCookie.left(semicolon);
}

} // namespace

/**
 * Integration tests for the login endpoints over a real socket.
 *
 * Step 13 delivers authentication but does not yet gate anything, so these tests
 * cover the credential exchange rather than access control — the gate and its
 * tests are step 14.
 *
 * The cases that matter are the ones that are invisible when it works: a wrong
 * password and a missing account must be indistinguishable, the cookie must carry
 * HttpOnly, and a correct login must not fall over because the hash runs on
 * another thread.
 */
class TestAuthServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void servesTheLoginPage();
    void loginPageIsSelfContained();
    void loginPageRefusesPost();

    void redirectsTrailingSlash();
    void redirectsCapitalisedPaths();
    void redirectPreservesTheNextParameter();
    void redirectPreservesThePostMethod();
    void doesNotClaimUnrelatedPaths();

    void acceptsCorrectCredentials();
    void issuesAnHttpOnlyCookie();
    void doesNotSetSecureOverPlainHttp();
    void rejectsTheWrongPassword();
    void rejectsAnUnknownUserIdentically();
    void rejectsADisabledAccount();
    void acceptsAnyCaseOfTheUsername();

    void rejectsAMalformedBody();
    void rejectsAMissingField();
    void rejectsGetOnTheLoginApi();
    void refusesAnOversizedBody();

    void rememberMeExtendsTheCookieLifetime();

    void sessionEndpointReportsAnonymous();
    void sessionEndpointReportsTheUser();
    void logoutInvalidatesTheSession();
    void logoutClearsTheCookieWithoutASession();

    void throttlesRepeatedFailures();

    void keepsServingOtherRequestsWhileHashing();

private:
    Response send(const QByteArray& raw, int timeoutMs = 30'000);
    Response postLogin(const QString& username, const QString& password,
                       bool remember = false);
    QByteArray loginAndGetToken(const QString& username, const QString& password);

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<db::Database>            m_database;
    std::unique_ptr<auth::UserStore>         m_users;
    std::unique_ptr<auth::SessionStore>      m_sessions;
    std::unique_ptr<auth::LoginRateLimiter>  m_limiter;
    std::unique_ptr<handlers::AuthHandler>   m_auth;
    std::unique_ptr<handlers::FileRouter>    m_router;
    std::unique_ptr<http::HttpServer>        m_server;
    quint16                                  m_port = 0;

    int m_ramId = 0;
    int m_disabledId = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestAuthServer::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.path();

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Album")));
    QFile marker(m_root + QStringLiteral("/marker.txt"));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.write("hello");
    marker.close();

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_root + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_users = std::make_unique<auth::UserStore>(m_database.get());

    // Accounts are created at a low work factor so the suite finishes in seconds.
    // Verification still costs whatever the stored hash says, which is the point:
    // the asynchronous path is exercised either way.
    m_users->setIterationsForTesting(1000);

    QString error;
    m_ramId = m_users->create(QStringLiteral("ram"), QStringLiteral("a-good-password"),
                              auth::Role::Admin, &error);
    QVERIFY2(m_ramId > 0, qPrintable(error));

    m_disabledId = m_users->create(QStringLiteral("retired"),
                                   QStringLiteral("a-good-password"),
                                   auth::Role::Family, &error);
    QVERIFY2(m_disabledId > 0, qPrintable(error));
    m_users->setEnabled(m_disabledId, false);

    m_sessions = std::make_unique<auth::SessionStore>(m_database.get());
    m_limiter  = std::make_unique<auth::LoginRateLimiter>();

    m_auth = std::make_unique<handlers::AuthHandler>(
        m_users.get(), m_sessions.get(), m_limiter.get());

    m_router = std::make_unique<handlers::FileRouter>(m_root);
    m_router->setAuthHandler(m_auth.get());

    m_server = std::make_unique<http::HttpServer>(m_router.get());
    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();
}

void TestAuthServer::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_router.reset();
    m_auth.reset();
    m_limiter.reset();
    m_sessions.reset();
    m_users.reset();
    m_database.reset();
}

void TestAuthServer::cleanup()
{
    // Each test starts with a clean throttle, or the first few would spend the
    // budget for the rest.
    if (m_limiter)
        m_limiter->clear();
}

// ─────────────────────────────────────────────────────────────────────────────

Response TestAuthServer::send(const QByteArray& raw, int timeoutMs)
{
    Response response;

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_port);
    if (!spinUntil([&socket] { return socket.state() == QAbstractSocket::ConnectedState; },
                   5000)) {
        return response;
    }

    socket.write(raw);
    socket.flush();

    readResponse(&socket, &response, timeoutMs);
    socket.disconnectFromHost();
    return response;
}

Response TestAuthServer::postLogin(const QString& username, const QString& password,
                                   bool remember)
{
    QJsonObject object;
    object[QStringLiteral("username")] = username;
    object[QStringLiteral("password")] = password;
    object[QStringLiteral("remember")] = remember;

    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QByteArray raw = "POST /api/login HTTP/1.1\r\n"
                     "Host: nas\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n";
    raw += body;

    return send(raw);
}

QByteArray TestAuthServer::loginAndGetToken(const QString& username,
                                            const QString& password)
{
    const Response response = postLogin(username, password);
    if (response.status != 200 || response.setCookies().isEmpty())
        return {};

    const QByteArray pair = cookiePair(response.setCookies().first());
    const qsizetype equals = pair.indexOf('=');
    return equals < 0 ? QByteArray() : pair.mid(equals + 1);
}

// ─────────────────────────────────────────────────────────────────────────────

void TestAuthServer::servesTheLoginPage()
{
    const Response response = send("GET /login HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 200);
    QVERIFY(response.header("content-type").startsWith("text/html"));

    // Proves the resource was actually linked in, not merely referenced.
    QVERIFY2(response.body.contains("NestDrive"), "The login page body is empty");
    QVERIFY(response.body.contains("/api/login"));

    // A cached login page serves no purpose and is worse on a shared device.
    QCOMPARE(response.header("cache-control"), QByteArray("no-store"));
}

void TestAuthServer::loginPageIsSelfContained()
{
    // It must not pull in app.css. That stylesheet is written for the virtual
    // scroller and puts the body into a fixed-height flex column with overflow
    // hidden; the sign-in page has no use for any of it and should not be exposed
    // to changes made for the grid.
    const Response response = send("GET /login HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");
    QCOMPARE(response.status, 200);

    // Asserted as "fetches no external stylesheet" rather than "does not mention
    // app.css": the page explains in a comment why it does not use it, and matching
    // the word caught that comment instead of a dependency.
    QVERIFY2(!response.body.contains("<link"),
             "The login page pulls in an external stylesheet");
    QVERIFY2(!response.body.contains("/_static/"),
             "The login page depends on a static asset");

    // And it must still be styled, or dropping the link just made it ugly.
    QVERIFY(response.body.contains("<style"));
}

void TestAuthServer::redirectsTrailingSlash()
{
    // A 404 here is indistinguishable from the sign-in page not existing.
    const Response response = send("GET /login/ HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 308);
    QCOMPARE(response.header("location"), QByteArray("/login"));
}

void TestAuthServer::redirectsCapitalisedPaths()
{
    // The case that cost a real evening: a phone keyboard autocapitalises, so a
    // hand-typed address arrives as /Login.
    for (const QByteArray& path : { QByteArrayLiteral("/Login"),
                                    QByteArrayLiteral("/LOGIN"),
                                    QByteArrayLiteral("/Login/") }) {
        const Response response = send("GET " + path + " HTTP/1.1\r\nHost: nas\r\n"
                                       "Connection: close\r\n\r\n");
        QVERIFY2(response.status == 308,
                 qPrintable(QStringLiteral("%1 gave %2")
                                .arg(QString::fromLatin1(path))
                                .arg(response.status)));
        QCOMPARE(response.header("location"), QByteArray("/login"));
    }

    const Response api = send("GET /API/Session HTTP/1.1\r\nHost: nas\r\n"
                              "Connection: close\r\n\r\n");
    QCOMPARE(api.status, 308);
    QCOMPARE(api.header("location"), QByteArray("/api/session"));
}

void TestAuthServer::redirectPreservesTheNextParameter()
{
    // Losing it would silently drop the user on the root instead of the folder they
    // asked for, which is the whole point of the parameter.
    const Response response = send("GET /Login?next=/Album HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 308);
    QCOMPARE(response.header("location"), QByteArray("/login?next=/Album"));
}

void TestAuthServer::redirectPreservesThePostMethod()
{
    // 308 rather than 301 exists for this: a 301 would be retried as GET by some
    // clients and the credentials would be dropped on the floor.
    const QByteArray body = R"({"username":"ram","password":"a-good-password"})";
    QByteArray raw = "POST /API/Login HTTP/1.1\r\nHost: nas\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + body;

    const Response response = send(raw);
    QCOMPARE(response.status, 308);
    QCOMPARE(response.header("location"), QByteArray("/api/login"));
}

void TestAuthServer::doesNotClaimUnrelatedPaths()
{
    // Folding must not spread. A file genuinely called "login" in the NAS root is
    // not the sign-in page, and "/" is not an auth path at all.
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("/")));
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("")));
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("/login.txt")));
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("/login/extra")));
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("/api")));
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("/api/list")));
    QVERIFY(!handlers::AuthHandler::handles(QByteArrayLiteral("/notlogin")));

    // The listing API must keep working, since it shares the /api prefix.
    const Response listing = send("GET /api/list HTTP/1.1\r\nHost: nas\r\n"
                                  "Connection: close\r\n\r\n");
    QCOMPARE(listing.status, 200);
}

void TestAuthServer::loginPageRefusesPost()
{
    const Response response = send("POST /login HTTP/1.1\r\nHost: nas\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n");
    QCOMPARE(response.status, 405);
}

void TestAuthServer::acceptsCorrectCredentials()
{
    const Response response = postLogin(QStringLiteral("ram"),
                                        QStringLiteral("a-good-password"));

    QCOMPARE(response.status, 200);

    const QJsonObject body = response.json();
    QCOMPARE(body.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("username")).toString(), QStringLiteral("ram"));
    QCOMPARE(body.value(QStringLiteral("role")).toString(), QStringLiteral("admin"));

    // The response must never echo the password back in any form.
    QVERIFY(!response.body.contains("a-good-password"));
}

void TestAuthServer::issuesAnHttpOnlyCookie()
{
    const Response response = postLogin(QStringLiteral("ram"),
                                        QStringLiteral("a-good-password"));
    QCOMPARE(response.status, 200);

    const QList<QByteArray> cookies = response.setCookies();
    QCOMPARE(cookies.size(), 1);

    const QByteArray cookie = cookies.first();
    QVERIFY2(cookie.startsWith("nd_session="), qPrintable(QString::fromLatin1(cookie)));

    // HttpOnly is what stops a stored-XSS bug becoming account takeover.
    QVERIFY2(cookie.contains("HttpOnly"), "The session cookie is readable by scripts");
    QVERIFY(cookie.contains("SameSite=Lax"));
    QVERIFY(cookie.contains("Path=/"));

    // And the token itself must be substantial, not a counter.
    const QByteArray pair = cookiePair(cookie);
    QVERIFY(pair.mid(pair.indexOf('=') + 1).size() >= 40);
}

void TestAuthServer::doesNotSetSecureOverPlainHttp()
{
    // Secure over plain HTTP would make the cookie unusable, which on a LAN
    // deployment with no certificate means nobody can log in at all.
    const Response response = postLogin(QStringLiteral("ram"),
                                        QStringLiteral("a-good-password"));
    QCOMPARE(response.status, 200);
    QVERIFY(!response.setCookies().first().contains("Secure"));
}

void TestAuthServer::rejectsTheWrongPassword()
{
    const Response response = postLogin(QStringLiteral("ram"),
                                        QStringLiteral("not-the-password"));

    QCOMPARE(response.status, 401);
    QVERIFY(response.setCookies().isEmpty());
}

void TestAuthServer::rejectsAnUnknownUserIdentically()
{
    const Response unknown = postLogin(QStringLiteral("nobody-at-all"),
                                       QStringLiteral("not-the-password"));
    const Response wrong   = postLogin(QStringLiteral("ram"),
                                       QStringLiteral("not-the-password"));

    // Identical status and identical body. Any difference here is a username
    // oracle: an attacker learns which accounts exist before guessing passwords.
    QCOMPARE(unknown.status, 401);
    QCOMPARE(wrong.status, 401);
    QCOMPARE(unknown.body, wrong.body);
}

void TestAuthServer::rejectsADisabledAccount()
{
    // The correct password for an account that has been turned off.
    const Response response = postLogin(QStringLiteral("retired"),
                                        QStringLiteral("a-good-password"));

    QCOMPARE(response.status, 401);
    QVERIFY(response.setCookies().isEmpty());

    // Indistinguishable from any other rejection.
    const Response wrong = postLogin(QStringLiteral("ram"),
                                     QStringLiteral("not-the-password"));
    QCOMPARE(response.body, wrong.body);
}

void TestAuthServer::acceptsAnyCaseOfTheUsername()
{
    for (const QString& variant : { QStringLiteral("RAM"),
                                    QStringLiteral("Ram"),
                                    QStringLiteral(" ram ") }) {
        const Response response = postLogin(variant, QStringLiteral("a-good-password"));
        QVERIFY2(response.status == 200,
                 qPrintable(QStringLiteral("%1 was rejected").arg(variant)));
    }
}

void TestAuthServer::rejectsAMalformedBody()
{
    const QByteArray body = "this is not json";
    QByteArray raw = "POST /api/login HTTP/1.1\r\nHost: nas\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + body;

    const Response response = send(raw);
    QCOMPARE(response.status, 400);
}

void TestAuthServer::rejectsAMissingField()
{
    const QByteArray body = R"({"username":"ram"})";
    QByteArray raw = "POST /api/login HTTP/1.1\r\nHost: nas\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + body;

    const Response response = send(raw);
    QCOMPARE(response.status, 400);
}

void TestAuthServer::rejectsGetOnTheLoginApi()
{
    const Response response = send("GET /api/login HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");
    QCOMPARE(response.status, 405);
}

void TestAuthServer::refusesAnOversizedBody()
{
    // Announced, not sent. The body is buffered whole, so the announcement alone
    // has to be refused — otherwise a client reserves memory it never fills.
    const qint64 huge = http::Connection::MaxRequestBodyBytes + 1;

    QByteArray raw = "POST /api/login HTTP/1.1\r\nHost: nas\r\n"
                     "Content-Length: " + QByteArray::number(huge) + "\r\n"
                     "Connection: close\r\n\r\n";

    const Response response = send(raw, 10'000);
    QCOMPARE(response.status, 413);
}

void TestAuthServer::rememberMeExtendsTheCookieLifetime()
{
    const Response brief = postLogin(QStringLiteral("ram"),
                                     QStringLiteral("a-good-password"), false);
    const Response lasting = postLogin(QStringLiteral("ram"),
                                       QStringLiteral("a-good-password"), true);

    QCOMPARE(brief.status, 200);
    QCOMPARE(lasting.status, 200);

    const QByteArray briefCookie   = brief.setCookies().first();
    const QByteArray lastingCookie = lasting.setCookies().first();

    QVERIFY(briefCookie.contains(
        "Max-Age=" + QByteArray::number(auth::SessionStore::DefaultIdleSeconds)));
    QVERIFY(lastingCookie.contains(
        "Max-Age=" + QByteArray::number(auth::SessionStore::RememberMeIdleSeconds)));
}

void TestAuthServer::sessionEndpointReportsAnonymous()
{
    const Response response = send("GET /api/session HTTP/1.1\r\nHost: nas\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 200);
    QCOMPARE(response.json().value(QStringLiteral("authenticated")).toBool(), false);

    // A nonsense cookie must read as anonymous, not as an error.
    const Response bogus = send("GET /api/session HTTP/1.1\r\nHost: nas\r\n"
                                "Cookie: nd_session=not-a-real-token\r\n"
                                "Connection: close\r\n\r\n");
    QCOMPARE(bogus.status, 200);
    QCOMPARE(bogus.json().value(QStringLiteral("authenticated")).toBool(), false);
}

void TestAuthServer::sessionEndpointReportsTheUser()
{
    const QByteArray token = loginAndGetToken(QStringLiteral("ram"),
                                              QStringLiteral("a-good-password"));
    QVERIFY(!token.isEmpty());

    const Response response = send("GET /api/session HTTP/1.1\r\nHost: nas\r\n"
                                   "Cookie: nd_session=" + token + "\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 200);

    const QJsonObject body = response.json();
    QCOMPARE(body.value(QStringLiteral("authenticated")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("username")).toString(), QStringLiteral("ram"));
    QCOMPARE(body.value(QStringLiteral("role")).toString(), QStringLiteral("admin"));
}

void TestAuthServer::logoutInvalidatesTheSession()
{
    const QByteArray token = loginAndGetToken(QStringLiteral("ram"),
                                              QStringLiteral("a-good-password"));
    QVERIFY(!token.isEmpty());

    const Response loggedOut = send("POST /api/logout HTTP/1.1\r\nHost: nas\r\n"
                                    "Cookie: nd_session=" + token + "\r\n"
                                    "Content-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
    QCOMPARE(loggedOut.status, 200);

    // The cookie is expired on the way out.
    QVERIFY(loggedOut.setCookies().first().contains("Max-Age=0"));

    // And the token is dead server-side, which is the part that actually matters:
    // clearing only the cookie would leave a copied token usable.
    const Response after = send("GET /api/session HTTP/1.1\r\nHost: nas\r\n"
                                "Cookie: nd_session=" + token + "\r\n"
                                "Connection: close\r\n\r\n");
    QCOMPARE(after.json().value(QStringLiteral("authenticated")).toBool(), false);
}

void TestAuthServer::logoutClearsTheCookieWithoutASession()
{
    const Response response = send("POST /api/logout HTTP/1.1\r\nHost: nas\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n");

    QCOMPARE(response.status, 200);
    QVERIFY(response.setCookies().first().contains("Max-Age=0"));
}

void TestAuthServer::throttlesRepeatedFailures()
{
    // Five wrong passwords, then the sixth attempt must be refused before any
    // hashing happens — otherwise guessing is only as slow as the hash.
    for (int i = 0; i < auth::LoginRateLimiter::MaxFailuresPerUser; ++i) {
        const Response response = postLogin(QStringLiteral("ram"),
                                            QStringLiteral("wrong-%1").arg(i));
        QCOMPARE(response.status, 401);
    }

    const Response blocked = postLogin(QStringLiteral("ram"),
                                       QStringLiteral("wrong-again"));
    QCOMPARE(blocked.status, 429);
    QVERIFY2(blocked.header("retry-after").toInt() > 0,
             "429 without a Retry-After leaves the client guessing");

    // The correct password is refused too while the lockout holds. Letting it
    // through would make the limit trivially bypassable by a dictionary that
    // happens to reach the real password.
    const Response evenCorrect = postLogin(QStringLiteral("ram"),
                                           QStringLiteral("a-good-password"));
    QCOMPARE(evenCorrect.status, 429);
}

void TestAuthServer::keepsServingOtherRequestsWhileHashing()
{
    // The reason verification runs on its own thread. A login in flight must not
    // stop the server answering anything else — on the reference hardware a hash
    // takes seconds, and blocking a worker for that long is a visible stall.
    QTcpSocket slow;
    slow.connectToHost(QHostAddress::LocalHost, m_port);
    QVERIFY(spinUntil([&slow] {
        return slow.state() == QAbstractSocket::ConnectedState;
    }, 5000));

    const QByteArray body =
        R"({"username":"ram","password":"a-good-password"})";
    QByteArray raw = "POST /api/login HTTP/1.1\r\nHost: nas\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + body;
    slow.write(raw);
    slow.flush();

    // Issued while the hash is still running.
    const Response listing = send("GET /api/list HTTP/1.1\r\nHost: nas\r\n"
                                  "Connection: close\r\n\r\n", 10'000);
    QCOMPARE(listing.status, 200);

    Response login;
    QVERIFY(readResponse(&slow, &login));
    QCOMPARE(login.status, 200);
}

QTEST_MAIN(TestAuthServer)
#include "tst_authserver.moc"
