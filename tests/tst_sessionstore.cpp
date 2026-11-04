#include <QTest>

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "auth/SessionStore.h"
#include "db/Database.h"

using auth::Session;
using auth::SessionStore;

/**
 * Tests for auth::SessionStore.
 *
 * Two properties carry the design. The token must never reach the database in
 * plain text, so that reading the file does not hand over live logins. And expiry
 * must actually expire — a session that outlives its window makes logout and
 * account disabling meaningless.
 *
 * Time is injected, because the default window is twelve hours.
 */
class TestSessionStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void issuesAValidatableToken();
    void storesOnlyTheHashOfTheToken();
    void issuesADifferentTokenEveryTime();
    void tokensAreLongEnough();
    void rejectsAnUnknownToken();
    void rejectsAnEmptyToken();
    void rejectsATamperedToken();
    void expiresAfterTheIdleWindow();
    void deletesTheRowOnceExpired();
    void refreshesExpiryOnUse();
    void throttlesTheRefreshWrite();
    void rememberMeLastsLonger();
    void destroyEndsTheSession();
    void destroyIsIdempotent();
    void destroyAllForUserLeavesOtherUsersAlone();
    void sweepRemovesOnlyExpiredRows();
    void refusesAnInvalidUser();

private:
    qint64 storedExpiry(const QByteArray& token) const
    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral(
            "SELECT expires_at FROM sessions WHERE token_hash = ?"));
        query.addBindValue(SessionStore::hashToken(token));
        if (!query.exec() || !query.next())
            return -1;
        return query.value(0).toLongLong();
    }

    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<db::Database>  m_database;
    std::unique_ptr<SessionStore>  m_sessions;
    qint64                         m_now = 1'700'000'000;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestSessionStore::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp->path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    // Real accounts, because sessions.user_id has a foreign key to users(id) and
    // the connection enforces it. Inserted directly rather than through UserStore:
    // this suite has nothing to say about password hashing, and paying for it here
    // would add seconds per test for no coverage.
    {
        auto handle = m_database->lock();
        for (int id : { 7, 8 }) {
            QSqlQuery query(handle.connection());
            query.prepare(QStringLiteral(
                "INSERT INTO users (id, username, pw_hash, role, enabled, created_at) "
                "VALUES (?, ?, 'x', 'family', 1, 0)"));
            query.addBindValue(id);
            query.addBindValue(QStringLiteral("user%1").arg(id));
            QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
        }
    }

    m_now      = 1'700'000'000;
    m_sessions = std::make_unique<SessionStore>(m_database.get());
    m_sessions->setClock([this] { return m_now; });
}

void TestSessionStore::cleanup()
{
    m_sessions.reset();
    m_database.reset();
    m_tmp.reset();
}

// ─────────────────────────────────────────────────────────────────────────────

void TestSessionStore::issuesAValidatableToken()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds,
        QStringLiteral("10.0.0.5"), QStringLiteral("curl/8"));

    QVERIFY(!token.isEmpty());

    const auto session = m_sessions->validate(token);
    QVERIFY(session.has_value());
    QCOMPARE(session->userId, 7);
    QCOMPARE(session->expiresAt, m_now + SessionStore::DefaultIdleSeconds);
}

void TestSessionStore::storesOnlyTheHashOfTheToken()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());
    QVERIFY(!token.isEmpty());

    // Scan every text column of the row: the token must appear in none of them.
    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral("SELECT token_hash, ip, user_agent FROM sessions")));
    QVERIFY(query.next());

    const QByteArray storedHash = query.value(0).toByteArray();
    QCOMPARE(storedHash, SessionStore::hashToken(token));
    QCOMPARE(storedHash.size(), 32);          // SHA-256.

    QVERIFY2(storedHash != token,
             "The session token is stored verbatim, so reading the database file "
             "hands over live sessions");
    QVERIFY(!query.value(1).toString().contains(QString::fromLatin1(token)));
    QVERIFY(!query.value(2).toString().contains(QString::fromLatin1(token)));
}

void TestSessionStore::issuesADifferentTokenEveryTime()
{
    QSet<QByteArray> tokens;
    for (int i = 0; i < 20; ++i) {
        tokens.insert(m_sessions->create(7, SessionStore::DefaultIdleSeconds,
                                         QStringLiteral("10.0.0.5"), QString()));
    }
    QCOMPARE(tokens.size(), 20);
}

void TestSessionStore::tokensAreLongEnough()
{
    const QByteArray token = SessionStore::generateToken();

    // 32 random bytes in unpadded base64url is 43 characters. Anything shorter
    // would mean the entropy was lost somewhere in the encoding.
    QCOMPARE(token.size(), 43);
    QVERIFY(!token.contains('='));
    QVERIFY(!token.contains('+'));
    QVERIFY(!token.contains('/'));
}

void TestSessionStore::rejectsAnUnknownToken()
{
    m_sessions->create(7, SessionStore::DefaultIdleSeconds,
                       QStringLiteral("10.0.0.5"), QString());

    QVERIFY(!m_sessions->validate(SessionStore::generateToken()).has_value());
}

void TestSessionStore::rejectsAnEmptyToken()
{
    QVERIFY(!m_sessions->validate(QByteArray()).has_value());
}

void TestSessionStore::rejectsATamperedToken()
{
    QByteArray token = m_sessions->create(7, SessionStore::DefaultIdleSeconds,
                                          QStringLiteral("10.0.0.5"), QString());
    QVERIFY(m_sessions->validate(token).has_value());

    // One flipped character must invalidate it. It would not if the lookup were a
    // prefix comparison rather than an exact one.
    QByteArray altered = token;
    altered[0] = (altered[0] == 'a') ? 'b' : 'a';
    QVERIFY(!m_sessions->validate(altered).has_value());

    // Truncation likewise.
    QVERIFY(!m_sessions->validate(token.left(token.size() - 1)).has_value());
    QVERIFY(!m_sessions->validate(token + "x").has_value());
}

void TestSessionStore::expiresAfterTheIdleWindow()
{
    // Two sessions, because the expiry is an idle window rather than an absolute
    // deadline: validating one extends it, so a single token cannot demonstrate
    // both the boundary and the lapse.
    const QByteArray used = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());
    const QByteArray untouched = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());

    m_now += SessionStore::DefaultIdleSeconds - 1;
    QVERIFY2(m_sessions->validate(used).has_value(), "Expired one second early");

    m_now += 2;

    QVERIFY2(!m_sessions->validate(untouched).has_value(),
             "An unused session outlived its idle window");

    // The other one survives precisely because it was used a moment ago. Idleness
    // is what expires a session, not age.
    QVERIFY2(m_sessions->validate(used).has_value(),
             "A session in active use was expired anyway");
}

void TestSessionStore::deletesTheRowOnceExpired()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());
    QCOMPARE(m_sessions->count(), 1);

    m_now += SessionStore::DefaultIdleSeconds + 1;
    QVERIFY(!m_sessions->validate(token).has_value());

    // Rejecting without deleting would let the table grow for every session ever
    // issued to a client that stopped coming back.
    QCOMPARE(m_sessions->count(), 0);
}

void TestSessionStore::refreshesExpiryOnUse()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());

    const qint64 originalExpiry = storedExpiry(token);

    // Past the throttle interval, so this use is allowed to write.
    m_now += SessionStore::RefreshIntervalSeconds + 1;

    const auto session = m_sessions->validate(token);
    QVERIFY(session.has_value());

    QVERIFY2(storedExpiry(token) > originalExpiry,
             "An active session was not extended, so it expires while in use");
    QCOMPARE(session->expiresAt, m_now + SessionStore::DefaultIdleSeconds);
}

void TestSessionStore::throttlesTheRefreshWrite()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());

    const qint64 originalExpiry = storedExpiry(token);

    // A folder view issues dozens of requests within a second. A write on each
    // would turn browsing into a burst of database traffic.
    m_now += 5;
    for (int i = 0; i < 30; ++i)
        QVERIFY(m_sessions->validate(token).has_value());

    QCOMPARE(storedExpiry(token), originalExpiry);
}

void TestSessionStore::rememberMeLastsLonger()
{
    const QByteArray brief = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());
    const QByteArray lasting = m_sessions->create(
        8, SessionStore::RememberMeIdleSeconds, QStringLiteral("10.0.0.6"), QString());

    m_now += SessionStore::DefaultIdleSeconds + 1;

    QVERIFY(!m_sessions->validate(brief).has_value());
    QVERIFY(m_sessions->validate(lasting).has_value());

    // And it is refreshed by its own window, not by the default one.
    QCOMPARE(m_sessions->validate(lasting)->expiresAt,
             m_now + SessionStore::RememberMeIdleSeconds);
}

void TestSessionStore::destroyEndsTheSession()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());

    QVERIFY(m_sessions->destroy(token));
    QVERIFY2(!m_sessions->validate(token).has_value(),
             "Logging out left the session usable");
    QCOMPARE(m_sessions->count(), 0);
}

void TestSessionStore::destroyIsIdempotent()
{
    const QByteArray token = m_sessions->create(
        7, SessionStore::DefaultIdleSeconds, QStringLiteral("10.0.0.5"), QString());

    QVERIFY(m_sessions->destroy(token));
    QVERIFY(!m_sessions->destroy(token));          // Nothing left to remove.
    QVERIFY(!m_sessions->destroy(QByteArray()));
}

void TestSessionStore::destroyAllForUserLeavesOtherUsersAlone()
{
    // Three devices for one person, one for someone else. Changing a password must
    // end the first three and none of the fourth.
    const QByteArray phone  = m_sessions->create(7, 3600, QStringLiteral("a"), QString());
    const QByteArray laptop = m_sessions->create(7, 3600, QStringLiteral("b"), QString());
    const QByteArray tablet = m_sessions->create(7, 3600, QStringLiteral("c"), QString());
    const QByteArray other  = m_sessions->create(8, 3600, QStringLiteral("d"), QString());

    QCOMPARE(m_sessions->destroyAllForUser(7), 3);

    QVERIFY(!m_sessions->validate(phone).has_value());
    QVERIFY(!m_sessions->validate(laptop).has_value());
    QVERIFY(!m_sessions->validate(tablet).has_value());
    QVERIFY(m_sessions->validate(other).has_value());
}

void TestSessionStore::sweepRemovesOnlyExpiredRows()
{
    const QByteArray brief   = m_sessions->create(7, 100, QStringLiteral("a"), QString());
    const QByteArray lasting = m_sessions->create(8, 10'000, QStringLiteral("b"), QString());

    m_now += 200;

    QCOMPARE(m_sessions->sweepExpired(), 1);
    QVERIFY(!m_sessions->validate(brief).has_value());
    QVERIFY(m_sessions->validate(lasting).has_value());
}

void TestSessionStore::refusesAnInvalidUser()
{
    QVERIFY(m_sessions->create(0, 3600, QStringLiteral("a"), QString()).isEmpty());
    QVERIFY(m_sessions->create(-1, 3600, QStringLiteral("a"), QString()).isEmpty());

    // A window of zero or less would issue a session that is already expired.
    QVERIFY(m_sessions->create(7, 0, QStringLiteral("a"), QString()).isEmpty());
    QVERIFY(m_sessions->create(7, -5, QStringLiteral("a"), QString()).isEmpty());

    // An account that does not exist must be refused by the foreign key. Without
    // that, deleting a user would leave their sessions behind as orphans that still
    // validate.
    QVERIFY2(m_sessions->create(9999, 3600, QStringLiteral("a"), QString()).isEmpty(),
             "A session was issued for a user id that does not exist");
    QCOMPARE(m_sessions->count(), 0);
}

QTEST_MAIN(TestSessionStore)
#include "tst_sessionstore.moc"
