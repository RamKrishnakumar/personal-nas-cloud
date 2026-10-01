#pragma once

#include <QList>
#include <QString>

#include <optional>

#include "auth/UserStore.h"

namespace auth {

/** Access level a folder grant confers. Mirrors the `perm` column. */
enum class PermLevel
{
    None,    ///< Explicitly denied. Not overridable by a shorter prefix.
    Read,
    Write,
};

QString          permLevelToString(PermLevel level);
std::optional<PermLevel> permLevelFromString(const QString& text);

/** One row of `folder_perms`, for one user. */
struct FolderPerm
{
    QString   pathPrefix;         ///< NAS-relative, leading slash, no trailing slash.
    PermLevel level = PermLevel::None;
};

/** What a requester may do at one path. */
struct Perm
{
    bool read   = false;
    bool write  = false;
    bool remove = false;
};

/**
 * @brief Normalises a NAS-relative path for comparison.
 *
 * Leading slash, no trailing slash, no doubled separators. `""`, `"/"` and `"//"`
 * all become `"/"`. Exposed because grants are normalised with the same function
 * they are matched with — two different normalisers would be a silent mismatch.
 */
QString normalisePermissionPath(const QString& path);

/**
 * @brief Decides what @p user may do at @p nasRelativePath.
 *
 * Pure: no filesystem, no database. That is what makes it exhaustively testable,
 * and a bug here is a disclosure bug that manual testing would not reveal.
 *
 * Rules, in order:
 *  1. A disabled account may do nothing.
 *  2. The longest matching `path_prefix` wins. `/Photos/2024` beats `/Photos`
 *     beats `/`.
 *  3. An explicit `none` denies, even beneath a permitting ancestor. Deny is not
 *     overridable by a shorter prefix.
 *  4. With no matching prefix: admin and family get read and write; **guest gets
 *     nothing**. Deny by default is what makes the guest role actually a guest.
 *  5. Removal requires write, and then: admin always, family only for their own
 *     uploads, guest never.
 *
 * @param perms          That user's rows, pre-loaded. Order does not matter.
 * @param fileOwnerId    Uploader of the file in question, when known. Absent for
 *                       a directory or an unrecorded file, which denies a family
 *                       member removal rather than granting it.
 *
 * @note Rule 5 deviates from the plan's literal "admin always": an administrator
 *       standing under an explicit `none` gets no removal either. Honouring the
 *       denial is the safer reading, and an explicit deny that silently did not
 *       apply to deletion would be a trap.
 */
Perm resolve(const UserRecord&        user,
             const QList<FolderPerm>& perms,
             const QString&           nasRelativePath,
             std::optional<int>       fileOwnerId = std::nullopt);

/**
 * @brief Everything the authentication gate established about one requester.
 *
 * Default-constructed means anonymous with no rights. That default is the reason
 * this is passed by value into handlers: a handler reached without the gate having
 * run sees "nobody", so the failure mode of a routing mistake is refusal rather
 * than disclosure.
 */
struct AuthContext
{
    UserRecord        user;            ///< id 0 when anonymous.
    QList<FolderPerm> folderPerms;
    bool              authenticated = false;

    Perm permissionFor(const QString&     nasRelativePath,
                       std::optional<int> fileOwnerId = std::nullopt) const
    {
        if (!authenticated)
            return {};
        return resolve(user, folderPerms, nasRelativePath, fileOwnerId);
    }
};

} // namespace auth
