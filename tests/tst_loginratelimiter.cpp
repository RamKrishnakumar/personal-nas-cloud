#include <QTest>

#include "auth/LoginRateLimiter.h"

using auth::LoginRateLimiter;
using auth::RateLimitDecision;

/**
 * Tests for auth::LoginRateLimiter.
 *
 * Driven by an injected clock, because the real window is fifteen minutes.
 *
 * The case that carries the design is the per-address bucket: without it, an
 * attacker spreads four guesses across a thousand usernames and never trips the
 * per-username limit, so the whole feature would be decoration.
 */
class TestLoginRateLimiter : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void allowsAFreshAttempt();
    void blocksAfterFiveFailuresForOneUser();
    void blocksAfterTwentyFailuresFromOneAddress();
    void spreadingAttemptsAcrossUsernamesStillTripsTheAddressLimit();
    void aDifferentUserIsUnaffected();
    void aDifferentAddressIsUnaffected();
    void countsCaseVariantsAsOneUsername();
    void allowsAgainOnceTheWindowPasses();
    void slidesRatherThanResettingWholesale();
    void successClearsTheUsernameBudget();
    void successDoesNotClearTheAddressBudget();
    void reportsHowLongToWait();
    void checkDoesNotRecordAnything();
    void boundsTheNumberOfTrackedUsernames();

private:
    void failTimes(int count, const QString& user, const QString& address)
    {
        for (int i = 0; i < count; ++i)
            m_limiter.recordFailure(user, address);
    }

    LoginRateLimiter m_limiter;
    qint64           m_now = 1'700'000'000;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestLoginRateLimiter::init()
{
    m_limiter.clear();
    m_now = 1'700'000'000;
    m_limiter.setClock([this] { return m_now; });
}

void TestLoginRateLimiter::allowsAFreshAttempt()
{
    QVERIFY(m_limiter.check(QStringLiteral("ram"), QStringLiteral("10.0.0.5")).allowed);
}

void TestLoginRateLimiter::blocksAfterFiveFailuresForOneUser()
{
    const QString user    = QStringLiteral("ram");
    const QString address = QStringLiteral("10.0.0.5");

    for (int i = 0; i < LoginRateLimiter::MaxFailuresPerUser; ++i) {
        QVERIFY2(m_limiter.check(user, address).allowed,
                 qPrintable(QStringLiteral("blocked early, at attempt %1").arg(i)));
        m_limiter.recordFailure(user, address);
    }

    QVERIFY(!m_limiter.check(user, address).allowed);
}

void TestLoginRateLimiter::blocksAfterTwentyFailuresFromOneAddress()
{
    const QString address = QStringLiteral("10.0.0.5");

    // Spread over distinct usernames so the per-username limit is never reached.
    for (int i = 0; i < LoginRateLimiter::MaxFailuresPerAddress; ++i)
        m_limiter.recordFailure(QStringLiteral("user%1").arg(i), address);

    QVERIFY(!m_limiter.check(QStringLiteral("fresh-name"), address).allowed);
}

void TestLoginRateLimiter::spreadingAttemptsAcrossUsernamesStillTripsTheAddressLimit()
{
    // The attack the address bucket exists for: four guesses each against many
    // accounts, which the per-username budget of five would never notice.
    const QString address = QStringLiteral("203.0.113.9");

    for (int account = 0; account < 6; ++account) {
        for (int attempt = 0; attempt < 4; ++attempt)
            m_limiter.recordFailure(QStringLiteral("victim%1").arg(account), address);
    }

    QCOMPARE(m_limiter.failuresForAddress(address), 24);
    QVERIFY2(!m_limiter.check(QStringLiteral("victim99"), address).allowed,
             "A username sweep from one address was not throttled");
}

void TestLoginRateLimiter::aDifferentUserIsUnaffected()
{
    failTimes(LoginRateLimiter::MaxFailuresPerUser,
              QStringLiteral("ram"), QStringLiteral("10.0.0.5"));

    QVERIFY(!m_limiter.check(QStringLiteral("ram"), QStringLiteral("10.0.0.5")).allowed);
    // Same address, but only five failures there — under the address limit.
    QVERIFY(m_limiter.check(QStringLiteral("someone-else"),
                            QStringLiteral("10.0.0.5")).allowed);
}

void TestLoginRateLimiter::aDifferentAddressIsUnaffected()
{
    for (int i = 0; i < LoginRateLimiter::MaxFailuresPerAddress; ++i)
        m_limiter.recordFailure(QStringLiteral("user%1").arg(i), QStringLiteral("10.0.0.5"));

    QVERIFY(!m_limiter.check(QStringLiteral("fresh"), QStringLiteral("10.0.0.5")).allowed);
    QVERIFY(m_limiter.check(QStringLiteral("fresh"), QStringLiteral("10.0.0.6")).allowed);
}

void TestLoginRateLimiter::countsCaseVariantsAsOneUsername()
{
    // Otherwise admin, Admin, ADMIN and aDmIn are four budgets for one account.
    const QString address = QStringLiteral("10.0.0.5");

    m_limiter.recordFailure(QStringLiteral("admin"), address);
    m_limiter.recordFailure(QStringLiteral("Admin"), address);
    m_limiter.recordFailure(QStringLiteral("ADMIN"), address);
    m_limiter.recordFailure(QStringLiteral("  admin  "), address);
    m_limiter.recordFailure(QStringLiteral("aDmIn"), address);

    QCOMPARE(m_limiter.failuresForUsername(QStringLiteral("admin")), 5);
    QVERIFY(!m_limiter.check(QStringLiteral("ADMIN"), address).allowed);
}

void TestLoginRateLimiter::allowsAgainOnceTheWindowPasses()
{
    const QString user    = QStringLiteral("ram");
    const QString address = QStringLiteral("10.0.0.5");

    failTimes(LoginRateLimiter::MaxFailuresPerUser, user, address);
    QVERIFY(!m_limiter.check(user, address).allowed);

    m_now += LoginRateLimiter::WindowSeconds + 1;
    QVERIFY2(m_limiter.check(user, address).allowed,
             "The lockout never lifts, so one burst locks an account forever");
}

void TestLoginRateLimiter::slidesRatherThanResettingWholesale()
{
    const QString user    = QStringLiteral("ram");
    const QString address = QStringLiteral("10.0.0.5");

    // Four failures, then a long gap, then one more. Only the last is inside the
    // window, so there should be four attempts of headroom left.
    failTimes(4, user, address);
    m_now += LoginRateLimiter::WindowSeconds + 1;
    m_limiter.recordFailure(user, address);

    QCOMPARE(m_limiter.failuresForUsername(user), 1);
    QVERIFY(m_limiter.check(user, address).allowed);
}

void TestLoginRateLimiter::successClearsTheUsernameBudget()
{
    const QString user    = QStringLiteral("ram");
    const QString address = QStringLiteral("10.0.0.5");

    failTimes(4, user, address);
    m_limiter.recordSuccess(user);

    QCOMPARE(m_limiter.failuresForUsername(user), 0);
    QVERIFY(m_limiter.check(user, address).allowed);
}

void TestLoginRateLimiter::successDoesNotClearTheAddressBudget()
{
    // One correct password does not make the preceding failures innocent, so the
    // address budget has to survive it.
    const QString address = QStringLiteral("10.0.0.5");

    for (int i = 0; i < LoginRateLimiter::MaxFailuresPerAddress; ++i)
        m_limiter.recordFailure(QStringLiteral("user%1").arg(i), address);

    m_limiter.recordSuccess(QStringLiteral("user0"));

    QCOMPARE(m_limiter.failuresForAddress(address),
             LoginRateLimiter::MaxFailuresPerAddress);
    QVERIFY(!m_limiter.check(QStringLiteral("user0"), address).allowed);
}

void TestLoginRateLimiter::reportsHowLongToWait()
{
    const QString user    = QStringLiteral("ram");
    const QString address = QStringLiteral("10.0.0.5");

    failTimes(LoginRateLimiter::MaxFailuresPerUser, user, address);

    const RateLimitDecision blocked = m_limiter.check(user, address);
    QVERIFY(!blocked.allowed);

    // All five failures happened at the same instant, so the budget frees a full
    // window later. The value becomes a Retry-After header, so it must be positive.
    QCOMPARE(blocked.retryAfterSeconds, LoginRateLimiter::WindowSeconds);

    m_now += LoginRateLimiter::WindowSeconds - 10;
    QCOMPARE(m_limiter.check(user, address).retryAfterSeconds, 10);
}

void TestLoginRateLimiter::checkDoesNotRecordAnything()
{
    // check() runs on every attempt before hashing. If it counted, calling it would
    // lock the account out by itself.
    const QString user    = QStringLiteral("ram");
    const QString address = QStringLiteral("10.0.0.5");

    for (int i = 0; i < 50; ++i)
        QVERIFY(m_limiter.check(user, address).allowed);

    QCOMPARE(m_limiter.failuresForUsername(user), 0);
    QCOMPARE(m_limiter.failuresForAddress(address), 0);
}

void TestLoginRateLimiter::boundsTheNumberOfTrackedUsernames()
{
    // An attacker inventing a username per attempt must not be able to grow the
    // table without limit. Tracking degrades; the address bucket still holds.
    const QString address = QStringLiteral("198.51.100.4");

    for (int i = 0; i < LoginRateLimiter::MaxTrackedUsernames + 500; ++i)
        m_limiter.recordFailure(QStringLiteral("throwaway-%1").arg(i), address);

    QVERIFY2(!m_limiter.check(QStringLiteral("anything"), address).allowed,
             "The address limit must still stop a username sweep");

    // Already-tracked names keep counting even once the table is full.
    QVERIFY(m_limiter.failuresForUsername(QStringLiteral("throwaway-0")) > 0);
}

QTEST_MAIN(TestLoginRateLimiter)
#include "tst_loginratelimiter.moc"
