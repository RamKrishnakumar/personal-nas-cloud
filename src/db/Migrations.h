#pragma once

#include <QSqlDatabase>
#include <QString>

namespace db {

/** Schema version this build expects. Bump when adding a migration step. */
constexpr int CurrentSchemaVersion = 4;

/**
 * @brief Brings @p connection's schema up to CurrentSchemaVersion.
 *
 * Version tracking uses SQLite's own `PRAGMA user_version`, so no bookkeeping
 * table is needed and the value travels with the file.
 *
 * Each step runs inside a transaction together with the `user_version` bump, so
 * an interrupted upgrade leaves the file at the previous version rather than
 * half-migrated. A database newer than this build is refused rather than
 * downgraded — silently dropping columns a later version added would destroy
 * data.
 *
 * Applying to an already-current database is a no-op, which is what makes it
 * safe to call on every startup.
 *
 * @param connection Open connection.
 * @param error      Set to a human-readable reason when the result is false.
 * @return true when the schema is at CurrentSchemaVersion afterwards.
 */
bool applyMigrations(QSqlDatabase& connection, QString* error);

/** Reads `PRAGMA user_version`, or -1 when it cannot be read. */
int readSchemaVersion(QSqlDatabase& connection);

} // namespace db
