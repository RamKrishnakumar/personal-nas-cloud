#include "auth/PermissionStore.h"

#include "db/Database.h"

#include <QSqlQuery>

namespace auth {

PermissionStore::PermissionStore(db::Database* database)
    : m_database(database)
{
}

QList<FolderPerm> PermissionStore::forUser(int userId) const
{
    QList<FolderPerm> grants;

    if (!m_database || !m_database->isOpen() || userId <= 0)
        return grants;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "SELECT path_prefix, perm FROM folder_perms WHERE user_id = ?"));
    query.addBindValue(userId);

    if (!query.exec())
        return grants;

    while (query.next()) {
        const auto level = permLevelFromString(query.value(1).toString());
        if (!level) {
            // Unreachable through this code thanks to the CHECK constraint. A
            // hand-edited row must not become an undefined grant, so it is dropped
            // rather than guessed at.
            continue;
        }

        grants.append({ normalisePermissionPath(query.value(0).toString()), *level });
    }

    return grants;
}

bool PermissionStore::grant(int userId, const QString& pathPrefix, PermLevel level)
{
    if (!m_database || !m_database->isOpen() || userId <= 0)
        return false;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO folder_perms (user_id, path_prefix, perm) "
        "VALUES (?, ?, ?)"));
    query.addBindValue(userId);
    query.addBindValue(normalisePermissionPath(pathPrefix));
    query.addBindValue(permLevelToString(level));

    return query.exec();
}

bool PermissionStore::revoke(int userId, const QString& pathPrefix)
{
    if (!m_database || !m_database->isOpen() || userId <= 0)
        return false;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "DELETE FROM folder_perms WHERE user_id = ? AND path_prefix = ?"));
    query.addBindValue(userId);
    query.addBindValue(normalisePermissionPath(pathPrefix));

    return query.exec() && query.numRowsAffected() > 0;
}

int PermissionStore::revokeAll(int userId)
{
    if (!m_database || !m_database->isOpen() || userId <= 0)
        return 0;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("DELETE FROM folder_perms WHERE user_id = ?"));
    query.addBindValue(userId);

    return query.exec() ? query.numRowsAffected() : 0;
}

} // namespace auth
