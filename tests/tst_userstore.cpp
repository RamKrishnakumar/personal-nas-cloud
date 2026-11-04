#include <QTest>

#include <QSqlQuery>
#include <QTemporaryDir>

#include "auth/PasswordHash.h"
#include "auth/UserStore.h"
#include "db/Database.h"

using auth::Role;
using auth::UserStore;

/**
 * Tests for auth::UserStore.
 *
 * The case worth the most attention is username normalisation. SQLite's UNIQUE
 * uses binary collation, so without folding, `Admin` and `admin` are two accounts
 * that read as one — and an attacker who can register the variant relies on nobody
 * looking closely.
 */
class TestUserStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void createsAnAccount();
    void findsByUsername();
    void findsById();
    void normalisesUsernameOnCreate();
    void findsRegardlessOfCase();
    void refusesACaseVariantOfAnExistingName();
    void refusesADuplicateName();
    void refusesAnEmptyName();
    void refusesAShortPassword();
    void storesAVerifiableHashAndNotThePassword();
    void roundTripsEveryRole();
    void updatesAPasswordHash();
    void disablesAnAccount();
    void recordsLoginTime();
    void countsAccounts();
    void returnsNothingForAnUnknownName();

private:
    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<db::Database>  m_database;
    std::unique_ptr<UserStore>     m_users;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestUserStore::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp->path() + QStringLiteral("/index.sqlite")),
             qPrintable(m_database->lastError()));

    m_users = std::make_unique<UserStore>(m_database.get());

    // The production work factor costs over a second per account, and this suite
    // creates twenty of them.
    m_users->setIterationsForTesting(1000);
}

void TestUserStore::cleanup()
{
    m_users.reset();
    m_database.reset();
    m_tmp.reset();
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUserStore::createsAnAccount()
{
    QString error;
    const int id = m_users->create(QStringLiteral("ram"),
                                   QStringLiteral("a-good-password"),
                                   Role::Admin, &error);

    QVERIFY2(id > 0, qPrintable(error));
    QVERIFY(error.isEmpty());
}

void TestUserStore::findsByUsername()
{
    QString error;
    const int id = m_users->create(QStringLiteral("ram"),
                                   QStringLiteral("a-good-password"),
                                   Role::Family, &error);
    QVERIFY2(id > 0, qPrintable(error));

    const auto found = m_users->findByUsername(QStringLiteral("ram"));
    QVERIFY(found.has_value());
    QCOMPARE(found->id, id);
    QCOMPARE(found->username, QStringLiteral("ram"));
    QCOMPARE(found->role, Role::Family);
    QVERIFY(found->enabled);
}

void TestUserStore::findsById()
{
    QString error;
    const int id = m_users->create(QStringLiteral("ram"),
                                   QStringLiteral("a-good-password"),
                                   Role::Guest, &error);
    QVERIFY(id > 0);

    const auto found = m_users->findById(id);
    QVERIFY(found.has_value());
    QCOMPARE(found->username, QStringLiteral("ram"));
    QCOMPARE(found->role, Role::Guest);

    QVERIFY(!m_users->findById(id + 999).has_value());
    QVERIFY(!m_users->findById(0).has_value());
    QVERIFY(!m_users->findById(-1).has_value());
}

void TestUserStore::normalisesUsernameOnCreate()
{
    QString error;
    QVERIFY(m_users->create(QStringLiteral("  RaM  "),
                            QStringLiteral("a-good-password"),
                            Role::Admin, &error) > 0);

    const auto found = m_users->findByUsername(QStringLiteral("ram"));
    QVERIFY(found.has_value());
    QCOMPARE(found->username, QStringLiteral("ram"));
}

void TestUserStore::findsRegardlessOfCase()
{
    QString error;
    QVERIFY(m_users->create(QStringLiteral("ram"),
                            QStringLiteral("a-good-password"),
                            Role::Admin, &error) > 0);

    for (const QString& variant : { QStringLiteral("RAM"),
                                    QStringLiteral("Ram"),
                                    QStringLiteral("rAm"),
                                    QStringLiteral("  ram ") }) {
        QVERIFY2(m_users->findByUsername(variant).has_value(),
                 qPrintable(QStringLiteral("could not find %1").arg(variant)));
    }
}

void TestUserStore::refusesACaseVariantOfAnExistingName()
{
    // The one that matters: allowing this creates a second "admin" nobody notices.
    QString error;
    QVERIFY(m_users->create(QStringLiteral("admin"),
                            QStringLiteral("a-good-password"),
                            Role::Admin, &error) > 0);

    QCOMPARE(m_users->create(QStringLiteral("Admin"),
                             QStringLiteral("another-password"),
                             Role::Guest, &error), 0);
    QVERIFY(!error.isEmpty());
    QCOMPARE(m_users->count(), 1);
}

void TestUserStore::refusesADuplicateName()
{
    QString error;
    QVERIFY(m_users->create(QStringLiteral("ram"),
                            QStringLiteral("a-good-password"),
                            Role::Admin, &error) > 0);

    QCOMPARE(m_users->create(QStringLiteral("ram"),
                             QStringLiteral("a-good-password"),
                             Role::Guest, &error), 0);
    QVERIFY(!error.isEmpty());
}

void TestUserStore::refusesAnEmptyName()
{
    QString error;
    QCOMPARE(m_users->create(QString(), QStringLiteral("a-good-password"),
                             Role::Admin, &error), 0);
    QCOMPARE(m_users->create(QStringLiteral("   "), QStringLiteral("a-good-password"),
                             Role::Admin, &error), 0);
    QVERIFY(!error.isEmpty());
}

void TestUserStore::refusesAShortPassword()
{
    QString error;
    const QString tooShort(auth::MinPasswordLength - 1, QLatin1Char('x'));

    QCOMPARE(m_users->create(QStringLiteral("ram"), tooShort, Role::Admin, &error), 0);
    QVERIFY(!error.isEmpty());
    QCOMPARE(m_users->count(), 0);
}

void TestUserStore::storesAVerifiableHashAndNotThePassword()
{
    const QString password = QStringLiteral("a-good-password");

    QString error;
    QVERIFY(m_users->create(QStringLiteral("ram"), password, Role::Admin, &error) > 0);

    const auto found = m_users->findByUsername(QStringLiteral("ram"));
    QVERIFY(found.has_value());

    // The stored value must be a hash, not the password, and not merely absent.
    QVERIFY(!found->passwordHash.isEmpty());
    QVERIFY2(!found->passwordHash.contains(password),
             "The plaintext password appears in the stored hash");
    QVERIFY(found->passwordHash.startsWith(QLatin1String("$pbkdf2-sha256$")));

    QVERIFY(auth::verifyPassword(password, found->passwordHash).ok());
    QVERIFY(!auth::verifyPassword(QStringLiteral("wrong-password"),
                                  found->passwordHash).ok());
}

void TestUserStore::roundTripsEveryRole()
{
    QString error;
    int index = 0;
    for (Role role : { Role::Admin, Role::Family, Role::Guest }) {
        const QString name = QStringLiteral("user%1").arg(index++);
        QVERIFY2(m_users->create(name, QStringLiteral("a-good-password"),
                                 role, &error) > 0, qPrintable(error));

        const auto found = m_users->findByUsername(name);
        QVERIFY(found.has_value());
        QCOMPARE(found->role, role);
    }
}

void TestUserStore::updatesAPasswordHash()
{
    QString error;
    const int id = m_users->create(QStringLiteral("ram"),
                                   QStringLiteral("a-good-password"),
                                   Role::Admin, &error);
    QVERIFY(id > 0);

    // Cheap work factor: this is testing the write, not the hashing.
    const QString replacement =
        auth::hashPassword(QStringLiteral("a-new-password"), 1000);
    QVERIFY(m_users->updatePasswordHash(id, replacement));

    const auto found = m_users->findById(id);
    QVERIFY(found.has_value());
    QCOMPARE(found->passwordHash, replacement);
    QVERIFY(auth::verifyPassword(QStringLiteral("a-new-password"),
                                 found->passwordHash).ok());
    QVERIFY(!auth::verifyPassword(QStringLiteral("a-good-password"),
                                  found->passwordHash).ok());

    // An empty replacement must be refused, or a failed hash would blank the
    // stored value and lock the account out permanently.
    QVERIFY(!m_users->updatePasswordHash(id, QString()));
}

void TestUserStore::disablesAnAccount()
{
    QString error;
    const int id = m_users->create(QStringLiteral("ram"),
                                   QStringLiteral("a-good-password"),
                                   Role::Admin, &error);
    QVERIFY(id > 0);

    m_users->setEnabled(id, false);
    const auto disabled = m_users->findById(id);
    QVERIFY(disabled.has_value());
    QVERIFY(!disabled->enabled);

    m_users->setEnabled(id, true);
    QVERIFY(m_users->findById(id)->enabled);
}

void TestUserStore::recordsLoginTime()
{
    QString error;
    const int id = m_users->create(QStringLiteral("ram"),
                                   QStringLiteral("a-good-password"),
                                   Role::Admin, &error);
    QVERIFY(id > 0);

    const auto lastLogin = [this] {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.exec(QStringLiteral("SELECT last_login_at FROM users"));
        query.next();
        return query.value(0);
    };

    QVERIFY(lastLogin().isNull());

    m_users->recordLogin(id);
    QVERIFY(lastLogin().toLongLong() > 0);
}

void TestUserStore::countsAccounts()
{
    QCOMPARE(m_users->count(), 0);

    QString error;
    for (int i = 0; i < 3; ++i) {
        QVERIFY(m_users->create(QStringLiteral("user%1").arg(i),
                                QStringLiteral("a-good-password"),
                                Role::Guest, &error) > 0);
    }

    QCOMPARE(m_users->count(), 3);
}

void TestUserStore::returnsNothingForAnUnknownName()
{
    QVERIFY(!m_users->findByUsername(QStringLiteral("nobody")).has_value());
    QVERIFY(!m_users->findByUsername(QString()).has_value());
}

QTEST_MAIN(TestUserStore)
#include "tst_userstore.moc"
