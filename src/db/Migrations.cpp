#include "db/Migrations.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

namespace {

/**
 * Schema version 1 — the v0.6 foundation.
 *
 * Covers users and roles, per-folder permissions, sessions, the thumbnail index,
 * upload ownership, scan progress and the audit log. Tables that later releases
 * consume (files, audit) are created now so their shape is fixed before code
 * depends on it.
 */
QStringList schemaVersion1()
{
    return {
        // ── Accounts ──────────────────────────────────────────────────────
        QStringLiteral(R"(
            CREATE TABLE users (
                id            INTEGER PRIMARY KEY,
                username      TEXT    NOT NULL UNIQUE,
                pw_hash       TEXT    NOT NULL,
                role          TEXT    NOT NULL
                              CHECK (role IN ('admin','family','guest')),
                enabled       INTEGER NOT NULL DEFAULT 1,
                created_at    INTEGER NOT NULL,
                last_login_at INTEGER
            )
        )"),

        // Longest matching path_prefix wins; guests have no implicit access.
        QStringLiteral(R"(
            CREATE TABLE folder_perms (
                user_id     INTEGER NOT NULL
                            REFERENCES users(id) ON DELETE CASCADE,
                path_prefix TEXT    NOT NULL,
                perm        TEXT    NOT NULL
                            CHECK (perm IN ('none','read','write')),
                PRIMARY KEY (user_id, path_prefix)
            )
        )"),

        // token_hash stores SHA-256 of the cookie value, never the value itself,
        // so a database leak does not hand over live sessions.
        QStringLiteral(R"(
            CREATE TABLE sessions (
                token_hash   BLOB    PRIMARY KEY,
                user_id      INTEGER NOT NULL
                             REFERENCES users(id) ON DELETE CASCADE,
                created_at   INTEGER NOT NULL,
                last_seen_at INTEGER NOT NULL,
                expires_at   INTEGER NOT NULL,
                ip           TEXT,
                user_agent   TEXT
            )
        )"),
        QStringLiteral("CREATE INDEX idx_sessions_expiry ON sessions(expires_at)"),

        // ── Thumbnail cache index ─────────────────────────────────────────
        // key embeds path, mtime and size, so a changed file yields a new key
        // and no invalidation logic is required.
        QStringLiteral(R"(
            CREATE TABLE thumbs (
                key          TEXT    PRIMARY KEY,
                src_path     TEXT    NOT NULL,
                mtime_ms     INTEGER NOT NULL,
                size         INTEGER NOT NULL,
                tier         TEXT    NOT NULL CHECK (tier IN ('s','m')),
                bytes        INTEGER NOT NULL,
                created_at   INTEGER NOT NULL,
                last_used_at INTEGER NOT NULL
            )
        )"),
        QStringLiteral("CREATE INDEX idx_thumbs_lru ON thumbs(last_used_at)"),

        // ── Upload ownership, for the "delete own files only" rule ────────
        QStringLiteral(R"(
            CREATE TABLE files (
                path        TEXT    PRIMARY KEY,
                uploaded_by INTEGER REFERENCES users(id) ON DELETE SET NULL,
                uploaded_at INTEGER NOT NULL
            )
        )"),

        // ── Background scan progress ──────────────────────────────────────
        QStringLiteral(R"(
            CREATE TABLE scan_state (
                path    TEXT    PRIMARY KEY,
                done_at INTEGER NOT NULL
            )
        )"),

        // ── Audit log ─────────────────────────────────────────────────────
        QStringLiteral(R"(
            CREATE TABLE audit (
                id      INTEGER PRIMARY KEY,
                ts      INTEGER NOT NULL,
                user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
                action  TEXT    NOT NULL,
                path    TEXT,
                bytes   INTEGER,
                status  INTEGER,
                ip      TEXT
            )
        )"),
        QStringLiteral("CREATE INDEX idx_audit_ts ON audit(ts)"),
    };
}

/** Runs one statement, reporting the failing SQL rather than just the driver text. */
bool runStatement(QSqlDatabase& connection, const QString& sql, QString* error)
{
    QSqlQuery query(connection);
    if (query.exec(sql))
        return true;

    if (error) {
        *error = QStringLiteral("%1\nwhile executing: %2")
                     .arg(query.lastError().text(), sql.trimmed());
    }
    return false;
}

} // namespace

namespace db {

int readSchemaVersion(QSqlDatabase& connection)
{
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
        return -1;

    bool ok = false;
    const int version = query.value(0).toInt(&ok);
    return ok ? version : -1;
}

bool applyMigrations(QSqlDatabase& connection, QString* error)
{
    const int current = readSchemaVersion(connection);
    if (current < 0) {
        if (error)
            *error = QStringLiteral("Could not read PRAGMA user_version.");
        return false;
    }

    if (current == CurrentSchemaVersion)
        return true;                            // Already current: no-op.

    if (current > CurrentSchemaVersion) {
        // Refuse rather than guess. A file written by a newer build may have
        // columns this one does not know about, and "migrating down" would mean
        // dropping them.
        if (error) {
            *error = QStringLiteral(
                         "Database schema version %1 is newer than this build "
                         "supports (%2). Upgrade NestDrive instead of "
                         "downgrading the database.")
                         .arg(current)
                         .arg(CurrentSchemaVersion);
        }
        return false;
    }

    // ── Apply each pending step, one transaction per version ──────────────
    // The user_version bump is inside the transaction with the step it belongs
    // to, so an interruption cannot leave a half-built schema claiming to be
    // complete.
    for (int target = current + 1; target <= CurrentSchemaVersion; ++target) {
        QStringList statements;
        switch (target) {
        case 1:
            statements = schemaVersion1();
            break;
        default:
            if (error)
                *error = QStringLiteral("No migration defined for version %1.").arg(target);
            return false;
        }

        if (!connection.transaction()) {
            if (error)
                *error = QStringLiteral("Could not begin transaction: %1")
                             .arg(connection.lastError().text());
            return false;
        }

        for (const QString& sql : statements) {
            if (!runStatement(connection, sql, error)) {
                connection.rollback();
                return false;
            }
        }

        // PRAGMA user_version does not accept a bound parameter, so the value is
        // interpolated. It is a loop counter bounded by CurrentSchemaVersion,
        // never external input.
        if (!runStatement(connection,
                          QStringLiteral("PRAGMA user_version = %1").arg(target),
                          error)) {
            connection.rollback();
            return false;
        }

        if (!connection.commit()) {
            if (error)
                *error = QStringLiteral("Could not commit migration to version %1: %2")
                             .arg(target)
                             .arg(connection.lastError().text());
            connection.rollback();
            return false;
        }
    }

    return readSchemaVersion(connection) == CurrentSchemaVersion;
}

} // namespace db
