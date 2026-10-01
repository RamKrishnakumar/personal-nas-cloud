#include "db/Database.h"

#include "db/Migrations.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace db {

Database::Database()
    // A unique connection name per instance lets tests open several databases at
    // once, and stops a second instance silently stealing the first's handle -
    // QSqlDatabase::addDatabase() replaces an existing connection of the same
    // name, closing it out from under its owner.
    : m_connectionName(QStringLiteral("nestdrive-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

Database::~Database()
{
    close();
}

bool Database::open(const QString& filePath)
{
    close();

    // ── Ensure the parent directory exists ────────────────────────────────
    const QFileInfo info(filePath);
    const QDir parent = info.absoluteDir();
    if (!parent.exists() && !QDir().mkpath(parent.absolutePath())) {
        m_lastError = QStringLiteral("Could not create directory: %1")
                          .arg(parent.absolutePath());
        return false;
    }

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        m_lastError = QStringLiteral(
            "The QSQLITE driver is not available. Qt was built without SQLite "
            "support, or the sqldrivers plugin is missing next to the executable.");
        return false;
    }

    m_connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_connection.setDatabaseName(info.absoluteFilePath());

    if (!m_connection.open()) {
        m_lastError = QStringLiteral("Could not open %1: %2")
                          .arg(info.absoluteFilePath(), m_connection.lastError().text());
        close();
        return false;
    }

    if (!applyPragmas()) {
        close();
        return false;
    }

    QString migrationError;
    if (!applyMigrations(m_connection, &migrationError)) {
        m_lastError = QStringLiteral("Schema migration failed: %1").arg(migrationError);
        // Leaving a connection open on a database whose schema is unknown invites
        // every later query to fail in a less obvious place.
        close();
        return false;
    }

    m_lastError.clear();
    return true;
}

bool Database::applyPragmas()
{
    // Each pragma is checked: a silently ignored one leaves the database running
    // with different durability or concurrency than the code assumes.
    struct Pragma { const char* sql; const char* expected; };
    static constexpr Pragma pragmas[] = {
        // Readers do not block the writer. This is what keeps a thumbnail lookup
        // from waiting on an audit insert.
        { "PRAGMA journal_mode = WAL",  "wal" },

        // Trades an fsync per commit for a small window of risk on power loss.
        // Acceptable: this database holds a cache, sessions and a log, none of
        // which is the user's actual data.
        { "PRAGMA synchronous = NORMAL", nullptr },

        // Without this, the ON DELETE clauses in the schema are decoration -
        // SQLite disables foreign keys per connection by default.
        { "PRAGMA foreign_keys = ON",    nullptr },

        // Wait rather than fail instantly if another writer holds the lock.
        { "PRAGMA busy_timeout = 5000",  nullptr },
    };

    for (const Pragma& pragma : pragmas) {
        QSqlQuery query(m_connection);
        if (!query.exec(QLatin1String(pragma.sql))) {
            m_lastError = QStringLiteral("Failed: %1 (%2)")
                              .arg(QLatin1String(pragma.sql), query.lastError().text());
            return false;
        }

        if (!pragma.expected)
            continue;

        // journal_mode returns the mode actually adopted, which can differ from
        // the one requested - WAL is unavailable on some network filesystems.
        if (!query.next()) {
            m_lastError = QStringLiteral("%1 returned no result")
                              .arg(QLatin1String(pragma.sql));
            return false;
        }

        const QString actual = query.value(0).toString().toLower();
        if (actual != QLatin1String(pragma.expected)) {
            m_lastError = QStringLiteral(
                              "%1 was not applied: expected '%2', got '%3'. "
                              "Is the database on a network drive?")
                              .arg(QLatin1String(pragma.sql),
                                   QLatin1String(pragma.expected),
                                   actual);
            return false;
        }
    }

    return true;
}

bool Database::isOpen() const
{
    return m_connection.isValid() && m_connection.isOpen();
}

void Database::close()
{
    if (m_connection.isValid()) {
        if (m_connection.isOpen())
            m_connection.close();

        // The QSqlDatabase copy must go out of scope before removeDatabase(),
        // or Qt warns that the connection is still referenced and leaks it.
        m_connection = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

int Database::schemaVersion() const
{
    if (!isOpen())
        return -1;

    QMutexLocker locker(&m_mutex);
    // readSchemaVersion needs a non-const connection; the mutex makes this safe.
    return readSchemaVersion(const_cast<QSqlDatabase&>(m_connection));
}

QString Database::filePath() const
{
    return m_connection.isValid() ? m_connection.databaseName() : QString();
}

QString Database::lastError() const
{
    return m_lastError;
}

} // namespace db
