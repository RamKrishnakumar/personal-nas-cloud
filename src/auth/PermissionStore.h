#pragma once

#include <QList>
#include <QString>

#include "auth/Permissions.h"

namespace db { class Database; }

namespace auth {

/**
 * @class PermissionStore
 * @brief Reads and writes `folder_perms`.
 *
 * Kept apart from the pure resolve() on purpose: the decision logic has to be
 * testable without a database, and the storage has to be testable without
 * reasoning about precedence.
 *
 * Thread-safe through db::Database.
 */
class PermissionStore
{
public:
    explicit PermissionStore(db::Database* database);

    /** Every grant for @p userId, normalised. Empty for a user with none. */
    QList<FolderPerm> forUser(int userId) const;

    /**
     * @brief Adds or replaces one grant.
     *
     * The prefix is normalised before storage, so `/Photos/` and `/Photos` cannot
     * become two rows that disagree.
     */
    bool grant(int userId, const QString& pathPrefix, PermLevel level);

    bool revoke(int userId, const QString& pathPrefix);

    /** Drops every grant for one user. */
    int revokeAll(int userId);

private:
    db::Database* m_database = nullptr;
};

} // namespace auth
