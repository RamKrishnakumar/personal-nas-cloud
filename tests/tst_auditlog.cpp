#include <QTest>

#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include "db/AuditLog.h"
#include "db/Database.h"

/**
 * Unit tests for db::AuditLog.
 *
 * The interesting cases are all about what gets stored as NULL rather than as a
 * number. `audit.user_id` is a foreign key, so "nobody" has to be NULL and not 0 —
 * SQLite rejects the insert outright for an id no user has, which would turn an
 * anonymous event into a lost row. The retention sweep is tested through an injected
 * clock, since the alternative is a test that waits ninety days.
 */
class TestAuditLog : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ── Writing ──────────────────────────────────────────────────────────
    void recordsAnEntryAndCountsIt();
    void storesEveryColumnItWasGiven();
    void anUnidentifiedActorIsStoredAsNull();
    void refusesAUserThatDoesNotExist();
    void notApplicableValuesAreStoredAsNull();
    void actionNamesAreStable();

    // ── Schema promises ──────────────────────────────────────────────────
    void deletingAUserKeepsTheirEntries();

    // ── Retention ────────────────────────────────────────────────────────
    void sweepRemovesOnlyEntriesPastRetention();
    void sweepIgnoresANonPositiveWindow();

    // ── Degraded ─────────────────────────────────────────────────────────
    void aClosedDatabaseFailsWithoutCrashing();

private:
    /** Inserts a user directly, because UserStore is not what is under test here. */
    int createUser(const QString& username);

    /** One column of the most recently written row. */
    QVariant lastValue(const QString& column);

    QTemporaryDir                 m_tmp;
    std::unique_ptr<db::Database> m_database;
    std::unique_ptr<db::AuditLog> m_audit;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestAuditLog::init()
{
    QVERIFY(m_tmp.isValid());

    m_database = std::make_unique<db::Database>();
    QVERIFY2(m_database->open(m_tmp.filePath(QStringLiteral("audit.sqlite"))),
             qPrintable(m_database->lastError()));

    m_audit = std::make_unique<db::AuditLog>(m_database.get());
}

void TestAuditLog::cleanup()
{
    m_audit.reset();
    m_database.reset();

    // A fresh file per test. Sharing one would make the retention tests depend on
    // what the writing tests happened to leave behind.
    QFile::remove(m_tmp.filePath(QStringLiteral("audit.sqlite")));
}

int TestAuditLog::createUser(const QString& username)
{
    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO users (username, pw_hash, role, enabled, created_at) "
        "VALUES (?, 'x', 'admin', 1, 0)"));
    query.addBindValue(username);

    if (!query.exec())
        return 0;
    return query.lastInsertId().toInt();
}

QVariant TestAuditLog::lastValue(const QString& column)
{
    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (!query.exec(QStringLiteral("SELECT %1 FROM audit ORDER BY id DESC LIMIT 1")
                        .arg(column))
        || !query.next()) {
        return {};
    }
    return query.value(0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Writing
// ─────────────────────────────────────────────────────────────────────────────

void TestAuditLog::recordsAnEntryAndCountsIt()
{
    QCOMPARE(m_audit->count(), 0);
    QVERIFY(m_audit->recordLoginFailure(0, QStringLiteral("10.0.0.9"), 401));
    QCOMPARE(m_audit->count(), 1);
}

void TestAuditLog::storesEveryColumnItWasGiven()
{
    const int userId = createUser(QStringLiteral("boss"));
    QVERIFY(userId > 0);

    QVERIFY(m_audit->recordDownload(userId, QStringLiteral("/Photos/a.jpg"),
                                    4096, QStringLiteral("192.168.0.5"), 200));

    QCOMPARE(lastValue(QStringLiteral("user_id")).toInt(), userId);
    QCOMPARE(lastValue(QStringLiteral("action")).toString(), QStringLiteral("download"));
    QCOMPARE(lastValue(QStringLiteral("path")).toString(),
             QStringLiteral("/Photos/a.jpg"));
    QCOMPARE(lastValue(QStringLiteral("bytes")).toLongLong(), 4096);
    QCOMPARE(lastValue(QStringLiteral("status")).toInt(), 200);
    QCOMPARE(lastValue(QStringLiteral("ip")).toString(), QStringLiteral("192.168.0.5"));

    // Not asserted against a fixed value, only that it was filled in — the point is
    // that a row without a timestamp is useless for auditing.
    QVERIFY(lastValue(QStringLiteral("ts")).toLongLong() > 0);
}

void TestAuditLog::anUnidentifiedActorIsStoredAsNull()
{
    // A failed login against a username nobody has. Zero would be a lie about which
    // account was involved, and the foreign key would reject it outright.
    QVERIFY(m_audit->recordLoginFailure(0, QStringLiteral("10.0.0.9"), 401));
    QVERIFY2(lastValue(QStringLiteral("user_id")).isNull(),
             "an unidentified actor must be NULL, not 0");
}

void TestAuditLog::refusesAUserThatDoesNotExist()
{
    // Not a defect to fix but a property to know about: the foreign key means a bad
    // id loses the row rather than storing a dangling reference. Every caller must
    // therefore pass 0 and not a guess.
    QVERIFY2(!m_audit->recordLogin(9999, QStringLiteral("10.0.0.9")),
             "an id belonging to no user must not be accepted");
    QCOMPARE(m_audit->count(), 0);
}

void TestAuditLog::notApplicableValuesAreStoredAsNull()
{
    const int userId = createUser(QStringLiteral("boss"));
    QVERIFY(m_audit->recordLogout(userId, QStringLiteral("10.0.0.9")));

    // A sign-out moves no data and names no path. NULL rather than 0 so that summing
    // or averaging bytes over the table does not quietly include events that never
    // transferred anything.
    QVERIFY(lastValue(QStringLiteral("bytes")).isNull());
    QVERIFY(lastValue(QStringLiteral("path")).isNull());
}

void TestAuditLog::actionNamesAreStable()
{
    // Stored as text and queried by name, so these strings are part of the database
    // format. Renaming one silently orphans every row already written with the old
    // spelling, which is why they are asserted rather than trusted.
    QCOMPARE(db::actionToString(db::Action::Login),      QStringLiteral("login"));
    QCOMPARE(db::actionToString(db::Action::LoginFail),  QStringLiteral("login_fail"));
    QCOMPARE(db::actionToString(db::Action::Logout),     QStringLiteral("logout"));
    QCOMPARE(db::actionToString(db::Action::List),       QStringLiteral("list"));
    QCOMPARE(db::actionToString(db::Action::Download),   QStringLiteral("download"));
    QCOMPARE(db::actionToString(db::Action::UserCreate), QStringLiteral("user_create"));
    QCOMPARE(db::actionToString(db::Action::Upload),     QStringLiteral("upload"));
    QCOMPARE(db::actionToString(db::Action::Delete),     QStringLiteral("delete"));
    QCOMPARE(db::actionToString(db::Action::Mkdir),      QStringLiteral("mkdir"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Schema promises
// ─────────────────────────────────────────────────────────────────────────────

void TestAuditLog::deletingAUserKeepsTheirEntries()
{
    // ON DELETE SET NULL, and it matters: an audit log that erased itself when an
    // account was removed would be useless for the case it is most wanted in, which
    // is working out what someone did before they were removed.
    const int userId = createUser(QStringLiteral("departed"));
    QVERIFY(m_audit->recordLogin(userId, QStringLiteral("10.0.0.9")));

    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral("DELETE FROM users WHERE id = ?"));
        query.addBindValue(userId);
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    }

    QCOMPARE(m_audit->count(), 1);
    QVERIFY2(lastValue(QStringLiteral("user_id")).isNull(),
             "the row should survive with a NULL user, not be deleted");
    QCOMPARE(lastValue(QStringLiteral("action")).toString(), QStringLiteral("login"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Retention
// ─────────────────────────────────────────────────────────────────────────────

void TestAuditLog::sweepRemovesOnlyEntriesPastRetention()
{
    constexpr qint64 day  = 24 * 60 * 60;
    constexpr qint64 base = 1'700'000'000;

    qint64 clock = base;
    m_audit->setClock([&clock] { return clock; });

    // One well past the window, one just inside it.
    clock = base;
    QVERIFY(m_audit->recordLoginFailure(0, QStringLiteral("10.0.0.1"), 401));

    clock = base + 89 * day;
    QVERIFY(m_audit->recordLoginFailure(0, QStringLiteral("10.0.0.2"), 401));

    clock = base + 100 * day;
    QCOMPARE(m_audit->sweepOlderThan(db::AuditLog::DefaultRetentionDays), 1);
    QCOMPARE(m_audit->count(), 1);

    // The survivor is the newer one, not whichever happened to be written last.
    QCOMPARE(lastValue(QStringLiteral("ip")).toString(), QStringLiteral("10.0.0.2"));
}

void TestAuditLog::sweepIgnoresANonPositiveWindow()
{
    // Zero days would otherwise mean "delete the entire log", which is a plausible
    // thing for a misconfigured retention setting to pass in.
    QVERIFY(m_audit->recordLoginFailure(0, QStringLiteral("10.0.0.1"), 401));
    QCOMPARE(m_audit->sweepOlderThan(0), 0);
    QCOMPARE(m_audit->sweepOlderThan(-5), 0);
    QCOMPARE(m_audit->count(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Degraded
// ─────────────────────────────────────────────────────────────────────────────

void TestAuditLog::aClosedDatabaseFailsWithoutCrashing()
{
    // Auditing must never be the thing that brings the application down. A closed
    // database is the realistic version of that: it happens when the profile
    // directory is not writable.
    m_database->close();

    QVERIFY(!m_audit->recordLogin(1, QStringLiteral("10.0.0.1")));
    QCOMPARE(m_audit->count(), 0);
    QCOMPARE(m_audit->sweepOlderThan(30), 0);
}

QTEST_GUILESS_MAIN(TestAuditLog)
#include "tst_auditlog.moc"
