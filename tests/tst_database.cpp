#include <QTest>

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QTemporaryDir>

#include "db/Database.h"
#include "db/Migrations.h"

/**
 * Tests for db::Database and db::applyMigrations.
 *
 * The behaviours worth guarding are the ones that fail quietly otherwise: a
 * pragma that did not take effect, a migration that ran twice, and a half-open
 * connection surviving a failed upgrade.
 */
class TestDatabase : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void opensAndMigrates();
    void createsMissingDirectories();
    void reopenIsIdempotent();
    void expectedTablesExist();
    void walModeIsActive();
    void foreignKeysAreEnforced();
    void roleConstraintIsEnforced();
    void cascadeDeleteRemovesDependents();
    void upgradesAnOlderDatabaseInPlace();
    void newerSchemaIsRefused();
    void failedOpenLeavesDatabaseClosed();
    void twoInstancesDoNotShareConnections();

private:
    QString freshPath(const QString& name);

    QTemporaryDir m_tmp;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestDatabase::initTestCase()
{
    QVERIFY2(m_tmp.isValid(), qPrintable(m_tmp.errorString()));
}

QString TestDatabase::freshPath(const QString& name)
{
    return QDir(m_tmp.path()).absoluteFilePath(name + QStringLiteral(".sqlite"));
}

void TestDatabase::opensAndMigrates()
{
    db::Database database;
    const QString path = freshPath(QStringLiteral("open"));

    QVERIFY2(database.open(path), qPrintable(database.lastError()));
    QVERIFY(database.isOpen());
    QCOMPARE(database.schemaVersion(), db::CurrentSchemaVersion);
    QVERIFY(QFile::exists(path));
}

void TestDatabase::createsMissingDirectories()
{
    // The database lives under AppDataLocation, which may not exist on a first
    // run. Failing there would make the very first launch fail.
    db::Database database;
    const QString path = QDir(m_tmp.path())
                             .absoluteFilePath(QStringLiteral("a/b/c/nested.sqlite"));

    QVERIFY2(database.open(path), qPrintable(database.lastError()));
    QVERIFY(QFile::exists(path));
}

void TestDatabase::reopenIsIdempotent()
{
    const QString path = freshPath(QStringLiteral("reopen"));

    {
        db::Database first;
        QVERIFY2(first.open(path), qPrintable(first.lastError()));
    }

    // Migrations run on every startup, so applying them to an already-current
    // database must be a no-op rather than a duplicate-table error.
    db::Database second;
    QVERIFY2(second.open(path), qPrintable(second.lastError()));
    QCOMPARE(second.schemaVersion(), db::CurrentSchemaVersion);
}

void TestDatabase::expectedTablesExist()
{
    db::Database database;
    QVERIFY2(database.open(freshPath(QStringLiteral("tables"))),
             qPrintable(database.lastError()));

    auto handle = database.lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral(
        "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")));

    QStringList tables;
    while (query.next())
        tables << query.value(0).toString();

    for (const QString& expected : { QStringLiteral("audit"),
                                     QStringLiteral("files"),
                                     QStringLiteral("folder_perms"),
                                     QStringLiteral("scan_state"),
                                     QStringLiteral("sessions"),
                                     QStringLiteral("thumb_fails"),
                                     QStringLiteral("thumbs"),
                                     QStringLiteral("users") }) {
        QVERIFY2(tables.contains(expected),
                 qPrintable(QStringLiteral("missing table %1; have: %2")
                                .arg(expected, tables.join(QStringLiteral(", ")))));
    }
}

void TestDatabase::walModeIsActive()
{
    // WAL is what lets a thumbnail lookup proceed while an audit row is written.
    // SQLite silently falls back on some filesystems, so assert it took effect.
    db::Database database;
    QVERIFY2(database.open(freshPath(QStringLiteral("wal"))),
             qPrintable(database.lastError()));

    auto handle = database.lock();
    QSqlQuery query(handle.connection());
    QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString().toLower(), QStringLiteral("wal"));
}

void TestDatabase::foreignKeysAreEnforced()
{
    // SQLite disables foreign keys per connection by default, which would make
    // every REFERENCES clause in the schema decorative.
    db::Database database;
    QVERIFY2(database.open(freshPath(QStringLiteral("fk"))),
             qPrintable(database.lastError()));

    auto handle = database.lock();
    QSqlQuery query(handle.connection());

    query.prepare(QStringLiteral(
        "INSERT INTO sessions (token_hash, user_id, created_at, last_seen_at, expires_at) "
        "VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(QByteArray("hash"));
    query.addBindValue(999);            // No such user.
    query.addBindValue(0);
    query.addBindValue(0);
    query.addBindValue(0);

    QVERIFY2(!query.exec(), "A session referencing a missing user must be rejected");

    // Assert the reason, not just the failure. A negative test that passes
    // because of a typo in the SQL proves nothing about foreign key enforcement.
    QVERIFY2(query.lastError().text().contains(QStringLiteral("FOREIGN KEY"),
                                               Qt::CaseInsensitive),
             qPrintable(QStringLiteral("Rejected, but not by the foreign key: %1")
                            .arg(query.lastError().text())));
}

void TestDatabase::roleConstraintIsEnforced()
{
    db::Database database;
    QVERIFY2(database.open(freshPath(QStringLiteral("role"))),
             qPrintable(database.lastError()));

    auto handle = database.lock();
    QSqlQuery query(handle.connection());

    const auto insertRole = [&](const QString& username, const QString& role) {
        query.prepare(QStringLiteral(
            "INSERT INTO users (username, pw_hash, role, created_at) VALUES (?, ?, ?, 0)"));
        query.addBindValue(username);
        query.addBindValue(QStringLiteral("hash"));
        query.addBindValue(role);
        return query.exec();
    };

    QVERIFY(insertRole(QStringLiteral("admin1"),  QStringLiteral("admin")));
    QVERIFY(insertRole(QStringLiteral("family1"), QStringLiteral("family")));
    QVERIFY(insertRole(QStringLiteral("guest1"),  QStringLiteral("guest")));

    // A typo'd role must not become a fourth, unhandled role at runtime.
    QVERIFY2(!insertRole(QStringLiteral("odd"), QStringLiteral("superuser")),
             "An unknown role must be rejected by the CHECK constraint");
    QVERIFY2(query.lastError().text().contains(QStringLiteral("CHECK"),
                                               Qt::CaseInsensitive),
             qPrintable(QStringLiteral("Rejected, but not by the CHECK constraint: %1")
                            .arg(query.lastError().text())));

    QVERIFY2(!insertRole(QStringLiteral("admin1"), QStringLiteral("admin")),
             "Usernames must be unique");
    QVERIFY2(query.lastError().text().contains(QStringLiteral("UNIQUE"),
                                               Qt::CaseInsensitive),
             qPrintable(QStringLiteral("Rejected, but not by the UNIQUE index: %1")
                            .arg(query.lastError().text())));
}

void TestDatabase::cascadeDeleteRemovesDependents()
{
    db::Database database;
    QVERIFY2(database.open(freshPath(QStringLiteral("cascade"))),
             qPrintable(database.lastError()));

    auto handle = database.lock();
    QSqlQuery query(handle.connection());

    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO users (id, username, pw_hash, role, created_at) "
        "VALUES (1, 'u', 'h', 'family', 0)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO folder_perms (user_id, path_prefix, perm) VALUES (1, '/Photos', 'read')")));

    // Deleting an account must not leave its permissions behind to be inherited
    // by whoever next receives that row id.
    QVERIFY(query.exec(QStringLiteral("DELETE FROM users WHERE id = 1")));
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM folder_perms")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
}

void TestDatabase::upgradesAnOlderDatabaseInPlace()
{
    // Every other test here starts from an empty file, so none of them exercises the
    // path a real installation takes: an existing database, with existing rows,
    // meeting a build that added a column. Getting this wrong does not show up on a
    // developer machine — only on somebody's actual data.
    const QString path = freshPath(QStringLiteral("upgrade"));

    {
        db::Database database;
        QVERIFY2(database.open(path), qPrintable(database.lastError()));

        auto handle = database.lock();
        QSqlQuery query(handle.connection());

        // A row that must survive the upgrade.
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO users (id, username, pw_hash, role, enabled, created_at) "
            "VALUES (1, 'keepme', 'hash', 'admin', 1, 100)")));

        // A swept directory, as a real installation would have hundreds of.
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO scan_state (path, done_at) VALUES ('E:/Photos', 100)")));

        // Wind the file back to version 2 by removing what 3 and 4 added, so this
        // covers a multi-step upgrade rather than only the most recent one.
        QVERIFY2(query.exec(QStringLiteral(
                     "ALTER TABLE scan_state DROP COLUMN caps")),
                 qPrintable(query.lastError().text()));
        QVERIFY2(query.exec(QStringLiteral(
                     "ALTER TABLE sessions DROP COLUMN idle_seconds")),
                 qPrintable(query.lastError().text()));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 2")));
    }

    db::Database upgraded;
    QVERIFY2(upgraded.open(path), qPrintable(upgraded.lastError()));
    QCOMPARE(upgraded.schemaVersion(), db::CurrentSchemaVersion);

    auto handle = upgraded.lock();
    QSqlQuery query(handle.connection());

    // Both columns are back, each with the default its migration declared.
    QVERIFY(query.exec(QStringLiteral("SELECT idle_seconds FROM sessions")));

    QVERIFY(query.exec(QStringLiteral("SELECT username FROM users WHERE id = 1")));
    QVERIFY2(query.next(), "The upgrade lost existing rows");
    QCOMPARE(query.value(0).toString(), QStringLiteral("keepme"));

    // The sweep record survives, and its capabilities come back empty. That empty
    // value is the whole mechanism: it matches no real fingerprint, so every
    // directory swept before the upgrade is swept once more — which is what makes
    // videos and HEIC get warmed in a library that was already fully scanned.
    QVERIFY(query.exec(QStringLiteral(
        "SELECT done_at, caps FROM scan_state WHERE path = 'E:/Photos'")));
    QVERIFY2(query.next(), "The upgrade lost the sweep records");
    QCOMPARE(query.value(0).toLongLong(), 100);
    QVERIFY2(query.value(1).toString().isEmpty(),
             "an upgraded sweep record must not claim any capabilities");
}

void TestDatabase::newerSchemaIsRefused()
{
    const QString path = freshPath(QStringLiteral("newer"));

    {
        db::Database database;
        QVERIFY2(database.open(path), qPrintable(database.lastError()));

        auto handle = database.lock();
        QSqlQuery query(handle.connection());
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = %1")
                               .arg(db::CurrentSchemaVersion + 5)));
    }

    // Opening a file from a future version must fail loudly. Migrating "down"
    // would mean dropping whatever that version added.
    db::Database database;
    QVERIFY2(!database.open(path), "A newer schema version must be refused");
    QVERIFY(database.lastError().contains(QStringLiteral("newer")));
}

void TestDatabase::failedOpenLeavesDatabaseClosed()
{
    // A half-open connection on an unknown schema pushes the failure into some
    // later query, where it is far harder to diagnose.
    const QString path = freshPath(QStringLiteral("failclosed"));

    {
        db::Database database;
        QVERIFY(database.open(path));
        auto handle = database.lock();
        QSqlQuery query(handle.connection());
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 99")));
    }

    db::Database database;
    QVERIFY(!database.open(path));
    QVERIFY2(!database.isOpen(), "A failed open must not leave the connection open");
    QCOMPARE(database.schemaVersion(), -1);
}

void TestDatabase::twoInstancesDoNotShareConnections()
{
    // QSqlDatabase::addDatabase() replaces any connection with the same name,
    // closing it under its owner. Unique names per instance prevent that.
    db::Database first;
    db::Database second;

    QVERIFY2(first.open(freshPath(QStringLiteral("inst1"))), qPrintable(first.lastError()));
    QVERIFY2(second.open(freshPath(QStringLiteral("inst2"))), qPrintable(second.lastError()));

    QVERIFY(first.isOpen());
    QVERIFY(second.isOpen());
    QVERIFY(first.filePath() != second.filePath());
}

// QSqlDatabase loads its driver as a plugin, which needs a QCoreApplication for
// plugin path resolution - QTEST_APPLESS_MAIN is not enough here.
QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
