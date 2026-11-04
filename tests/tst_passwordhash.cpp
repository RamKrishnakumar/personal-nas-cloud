#include <QTest>

#include <QElapsedTimer>
#include <QSet>

#include "auth/PasswordHash.h"

using namespace auth;

namespace {

/**
 * Work factor for tests.
 *
 * The production value takes well over a second per call, which would make this
 * suite take minutes. verifyPassword() reads the count out of the hash, so a cheap
 * hash still exercises every code path — and the needsUpgrade flag besides.
 */
constexpr int TestIterations = 1'000;

} // namespace

/**
 * Tests for auth::PasswordHash.
 *
 * The properties that matter are not "does it hash" but: two users with the same
 * password get different stored values, a wrong password is rejected, a corrupted
 * stored value is reported rather than treated as a match, and the decoy used to
 * equalise login timing is itself a valid hash. The last one is easy to get wrong
 * and silently defeats the username-enumeration defence.
 */
class TestPasswordHash : public QObject
{
    Q_OBJECT

private slots:
    void hashHasTheExpectedShape();
    void acceptsTheCorrectPassword();
    void rejectsTheWrongPassword();
    void saltsEveryHashDifferently();
    void rejectsAnEmptyPassword();
    void rejectsAnOverlongPassword();
    void acceptsAnOverlongCandidateWithoutHashing();
    void handlesUnicodePasswords();

    void reportsMalformedStoredValues_data();
    void reportsMalformedStoredValues();

    void flagsAWeakerStoredHashForUpgrade();
    void doesNotFlagACurrentHash();

    void decoyIsAUsableHash();
    void decoyNeverMatches();

    void constantTimeEqualsComparesContent();
};

// ─────────────────────────────────────────────────────────────────────────────

void TestPasswordHash::hashHasTheExpectedShape()
{
    const QString encoded = hashPassword(QStringLiteral("hunter2hunter2"), TestIterations);

    QVERIFY(!encoded.isEmpty());
    QVERIFY2(encoded.startsWith(QLatin1String("$pbkdf2-sha256$i=1000$")),
             qPrintable(encoded));

    const QStringList fields = encoded.split(QLatin1Char('$'));
    QCOMPARE(fields.size(), 5);

    // Only the salt and key are checked for base64 artefacts. The iteration field
    // legitimately contains '=', which is why the separator is '$' and not '='.
    for (const QString& field : { fields.at(3), fields.at(4) }) {
        QVERIFY2(!field.contains(QLatin1Char('=')), qPrintable(field));
        QVERIFY2(!field.contains(QLatin1Char('+')), qPrintable(field));
        QVERIFY2(!field.contains(QLatin1Char('/')), qPrintable(field));
    }
}

void TestPasswordHash::acceptsTheCorrectPassword()
{
    const QString password = QStringLiteral("correct horse battery staple");
    const QString encoded  = hashPassword(password, TestIterations);

    const VerifyResult result = verifyPassword(password, encoded);
    QCOMPARE(result.outcome, VerifyOutcome::Ok);
    QVERIFY(result.ok());
}

void TestPasswordHash::rejectsTheWrongPassword()
{
    const QString encoded = hashPassword(QStringLiteral("hunter2hunter2"), TestIterations);

    for (const QString& wrong : { QStringLiteral("hunter2hunter3"),
                                  QStringLiteral("hunter2hunter"),
                                  QStringLiteral("HUNTER2HUNTER2"),
                                  QStringLiteral(" hunter2hunter2") }) {
        const VerifyResult result = verifyPassword(wrong, encoded);
        QCOMPARE(result.outcome, VerifyOutcome::Mismatch);
        QVERIFY(!result.ok());
    }
}

void TestPasswordHash::saltsEveryHashDifferently()
{
    // Without a per-hash salt, two users with the same password share a stored
    // value — which both leaks that fact and makes one cracked hash crack both.
    const QString password = QStringLiteral("shared password");

    QSet<QString> seen;
    for (int i = 0; i < 5; ++i)
        seen.insert(hashPassword(password, TestIterations));

    QCOMPARE(seen.size(), 5);

    // All of them must still verify.
    for (const QString& encoded : seen)
        QVERIFY(verifyPassword(password, encoded).ok());
}

void TestPasswordHash::rejectsAnEmptyPassword()
{
    QVERIFY(hashPassword(QString(), TestIterations).isEmpty());
    QVERIFY(hashPassword(QStringLiteral(""), TestIterations).isEmpty());
}

void TestPasswordHash::rejectsAnOverlongPassword()
{
    const QString huge(MaxPasswordBytes + 1, QLatin1Char('x'));
    QVERIFY2(hashPassword(huge, TestIterations).isEmpty(),
             "An unbounded password is an unbounded amount of hashing work");
}

void TestPasswordHash::acceptsAnOverlongCandidateWithoutHashing()
{
    // It cannot be correct, since no such password can be stored. It must still
    // come back as a mismatch rather than as a malformed stored value, because the
    // stored value is fine — the candidate is not.
    const QString encoded = hashPassword(QStringLiteral("hunter2hunter2"), TestIterations);
    const QString huge(MaxPasswordBytes + 1, QLatin1Char('x'));

    QCOMPARE(verifyPassword(huge, encoded).outcome, VerifyOutcome::Mismatch);
}

void TestPasswordHash::handlesUnicodePasswords()
{
    // Hashing works on UTF-8 bytes, so a passphrase outside ASCII must round-trip.
    const QString password = QStringLiteral("हिन्दी-пароль-🔐");
    const QString encoded  = hashPassword(password, TestIterations);

    QVERIFY(verifyPassword(password, encoded).ok());
    QCOMPARE(verifyPassword(QStringLiteral("हिन्दी-пароль"), encoded).outcome,
             VerifyOutcome::Mismatch);
}

void TestPasswordHash::reportsMalformedStoredValues_data()
{
    QTest::addColumn<QString>("stored");

    QTest::newRow("empty")            << QString();
    QTest::newRow("no separators")    << QStringLiteral("not a hash at all");
    QTest::newRow("too few fields")   << QStringLiteral("$pbkdf2-sha256$i=1000$salt");
    QTest::newRow("too many fields")  << QStringLiteral("$pbkdf2-sha256$i=1$a$b$c");
    QTest::newRow("no leading $")     << QStringLiteral("pbkdf2-sha256$i=1000$YWJj$ZGVm");
    QTest::newRow("unknown algo")     << QStringLiteral("$argon2id$i=1000$YWJj$ZGVm");
    QTest::newRow("no i= prefix")     << QStringLiteral("$pbkdf2-sha256$1000$YWJj$ZGVm");
    QTest::newRow("iterations text")  << QStringLiteral("$pbkdf2-sha256$i=lots$YWJj$ZGVm");
    QTest::newRow("zero iterations")  << QStringLiteral("$pbkdf2-sha256$i=0$YWJj$ZGVm");
    QTest::newRow("empty salt")       << QStringLiteral("$pbkdf2-sha256$i=1000$$ZGVm");
    QTest::newRow("short key")        << QStringLiteral("$pbkdf2-sha256$i=1000$YWJj$ZGVm");
}

void TestPasswordHash::reportsMalformedStoredValues()
{
    QFETCH(QString, stored);

    // Never Ok. A stored value this build cannot read must fail closed — treating
    // it as a match would turn a corrupted row into a free login.
    const VerifyResult result = verifyPassword(QStringLiteral("anything"), stored);
    QCOMPARE(result.outcome, VerifyOutcome::Malformed);
    QVERIFY(!result.ok());
}

void TestPasswordHash::flagsAWeakerStoredHashForUpgrade()
{
    const QString password = QStringLiteral("hunter2hunter2");
    const QString weak     = hashPassword(password, TestIterations);

    const VerifyResult result = verifyPassword(password, weak);
    QVERIFY(result.ok());
    QVERIFY2(result.needsUpgrade,
             "A hash below the current work factor must be marked for re-hashing");
}

void TestPasswordHash::doesNotFlagACurrentHash()
{
    // The only test that pays the real work factor. Worth it: it is the one check
    // that the production parameter and the comparison agree.
    QElapsedTimer timer;
    timer.start();

    const QString password = QStringLiteral("hunter2hunter2");
    const QString strong   = hashPassword(password);

    const VerifyResult result = verifyPassword(password, strong);
    QVERIFY(result.ok());
    QVERIFY(!result.needsUpgrade);

    // Not an assertion about speed, a guard against the work factor being
    // accidentally reduced to something a GPU walks through.
    QVERIFY2(timer.elapsed() > 100,
             qPrintable(QStringLiteral("two hashes at the production work factor "
                                       "took only %1 ms").arg(timer.elapsed())));
}

void TestPasswordHash::decoyIsAUsableHash()
{
    // The whole point of the decoy is that verifying against it costs what a real
    // verification costs. If it were malformed, verifyPassword() would return
    // immediately and an attacker could tell missing accounts apart by timing.
    const VerifyResult result = verifyPassword(QStringLiteral("anything"), decoyHash());

    QVERIFY2(result.outcome != VerifyOutcome::Malformed,
             "The decoy hash is malformed, so it costs nothing to verify and the "
             "username-enumeration defence does not work");
    QCOMPARE(result.outcome, VerifyOutcome::Mismatch);
}

void TestPasswordHash::decoyNeverMatches()
{
    for (const QString& attempt : { QStringLiteral(""),
                                    QStringLiteral("password"),
                                    QStringLiteral("admin"),
                                    QStringLiteral("decoy") }) {
        QVERIFY(!verifyPassword(attempt, decoyHash()).ok());
    }
}

void TestPasswordHash::constantTimeEqualsComparesContent()
{
    QVERIFY(constantTimeEquals(QByteArray("abc"), QByteArray("abc")));
    QVERIFY(!constantTimeEquals(QByteArray("abc"), QByteArray("abd")));
    QVERIFY(!constantTimeEquals(QByteArray("abc"), QByteArray("abcd")));
    QVERIFY(constantTimeEquals(QByteArray(), QByteArray()));

    // Embedded NULs must not shorten the comparison.
    QVERIFY(constantTimeEquals(QByteArray("a\0b", 3), QByteArray("a\0b", 3)));
    QVERIFY(!constantTimeEquals(QByteArray("a\0b", 3), QByteArray("a\0c", 3)));
}

QTEST_MAIN(TestPasswordHash)
#include "tst_passwordhash.moc"
