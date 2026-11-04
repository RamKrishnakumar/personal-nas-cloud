#pragma once

#include <QMutex>
#include <QSqlDatabase>
#include <QString>

namespace db {

/**
 * @class Database
 * @brief The application's single SQLite connection, serialised by a mutex.
 *
 * ### Why one connection rather than one per thread
 *
 * QSqlDatabase connections cannot be shared across threads, so the usual
 * alternative is a named connection per worker. At NAS scale — a handful of
 * users and single-digit requests per second — that buys nothing and costs a
 * pool of connections, a matching set of prepared statements, and a rule every
 * future caller has to remember. One connection behind a mutex is smaller, uses
 * less memory on the reference hardware, and cannot be misused across threads by
 * accident.
 *
 * The SQL this application runs is short: session lookups, thumbnail index
 * hits, an audit insert. Nothing holds the mutex long enough to matter. If that
 * ever changes, WAL mode is already enabled, so moving to a reader connection
 * per worker is a local change.
 *
 * ### Usage
 *
 * All access goes through lock(), which hands back an accessor that holds the
 * mutex for its own lifetime:
 *
 * @code
 * auto handle = database.lock();
 * QSqlQuery query(handle.connection());
 * query.prepare(QStringLiteral("SELECT role FROM users WHERE username = ?"));
 * query.addBindValue(username);
 * query.exec();
 * @endcode
 *
 * Keeping the mutex tied to the accessor's lifetime is deliberate: a QSqlQuery
 * outliving the lock would touch the connection unguarded, and that is the one
 * mistake this design has to prevent.
 */
class Database
{
    Q_DISABLE_COPY_MOVE(Database)

public:
    /**
     * @brief Scoped, exclusive access to the connection.
     *
     * Holds the database mutex from construction to destruction. Do not let a
     * QSqlQuery built from connection() outlive the accessor.
     */
    class Handle
    {
        Q_DISABLE_COPY_MOVE(Handle)

    public:
        explicit Handle(QMutex& mutex, QSqlDatabase& connection)
            : m_mutex(mutex), m_connection(connection) { m_mutex.lock(); }

        ~Handle() { m_mutex.unlock(); }

        QSqlDatabase& connection() { return m_connection; }

    private:
        QMutex&       m_mutex;
        QSqlDatabase& m_connection;
    };

    Database();
    ~Database();

    /**
     * @brief Opens @p filePath, creating it if absent, and applies migrations.
     *
     * Creates any missing parent directories. Sets the connection pragmas
     * described in the v0.6 plan §7, then brings the schema up to the current
     * version.
     *
     * @return true on success. On failure the connection is closed again and
     *         lastError() explains why — a half-open database is worse than none.
     */
    bool open(const QString& filePath);

    bool isOpen() const;
    void close();

    /** Schema version recorded in the file, or -1 when not open. */
    int schemaVersion() const;

    /** Path the connection was opened against, empty when not open. */
    QString filePath() const;

    /** Reason for the most recent failure, or an empty string. */
    QString lastError() const;

    /** Exclusive access to the connection. See the class documentation. */
    Handle lock() { return Handle(m_mutex, m_connection); }

private:
    bool applyPragmas();

    QSqlDatabase m_connection;
    QString      m_connectionName;   ///< Unique, so several instances can coexist.
    QString      m_lastError;
    mutable QMutex m_mutex;
};

} // namespace db
