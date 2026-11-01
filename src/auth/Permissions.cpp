#include "auth/Permissions.h"

#include <QDir>

namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity PathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity PathCase = Qt::CaseSensitive;
#endif

/**
 * @brief True when @p path is @p prefix or lies beneath it.
 *
 * The separator is appended before the prefix test deliberately: without it a
 * grant on `/Photo` would also cover the unrelated `/Photos`, which is how a
 * permission system quietly leaks a whole folder.
 */
bool prefixCovers(const QString& prefix, const QString& path)
{
    if (prefix == QLatin1String("/"))
        return true;                       // Root covers everything.

    if (path.compare(prefix, PathCase) == 0)
        return true;

    return path.startsWith(prefix + QLatin1Char('/'), PathCase);
}

} // namespace

namespace auth {

QString permLevelToString(PermLevel level)
{
    switch (level) {
    case PermLevel::None:  return QStringLiteral("none");
    case PermLevel::Read:  return QStringLiteral("read");
    case PermLevel::Write: return QStringLiteral("write");
    }
    return QStringLiteral("none");
}

std::optional<PermLevel> permLevelFromString(const QString& text)
{
    if (text == QLatin1String("none"))  return PermLevel::None;
    if (text == QLatin1String("read"))  return PermLevel::Read;
    if (text == QLatin1String("write")) return PermLevel::Write;
    return std::nullopt;
}

QString normalisePermissionPath(const QString& path)
{
    // cleanPath collapses doubled separators and resolves "." and ".." textually.
    // Anything that escapes upward is clamped to the root rather than allowed to
    // become a shorter prefix than intended.
    QString cleaned = QDir::cleanPath(path.isEmpty() ? QStringLiteral("/") : path);

    cleaned.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (!cleaned.startsWith(QLatin1Char('/')))
        cleaned.prepend(QLatin1Char('/'));

    while (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/')))
        cleaned.chop(1);

    // A path that climbed out of the root is meaningless here; treat it as the
    // root, which is the most restrictive interpretation for a guest.
    if (cleaned.startsWith(QLatin1String("/..")))
        return QStringLiteral("/");

    return cleaned;
}

Perm resolve(const UserRecord&        user,
             const QList<FolderPerm>& perms,
             const QString&           nasRelativePath,
             std::optional<int>       fileOwnerId)
{
    Perm result;

    // Rule 1. Checked before anything else, so disabling an account is absolute.
    if (!user.enabled || user.id <= 0)
        return result;

    const QString path = normalisePermissionPath(nasRelativePath);

    // Rule 2. Longest matching prefix wins, measured after normalisation so that
    // a grant written as "/Photos/" cannot beat one written as "/Photos/2024".
    const FolderPerm* winner = nullptr;
    qsizetype         winningLength = -1;

    for (const FolderPerm& candidate : perms) {
        const QString prefix = normalisePermissionPath(candidate.pathPrefix);
        if (!prefixCovers(prefix, path))
            continue;

        const qsizetype length = prefix == QLatin1String("/") ? 0 : prefix.size();
        if (length > winningLength) {
            winningLength = length;
            winner        = &candidate;
        }
    }

    if (winner) {
        // Rule 3. An explicit denial is the end of the matter.
        switch (winner->level) {
        case PermLevel::None:
            return result;
        case PermLevel::Read:
            result.read = true;
            break;
        case PermLevel::Write:
            result.read  = true;
            result.write = true;
            break;
        }
    } else {
        // Rule 4. Deny by default for guests; the trusted roles get the library.
        switch (user.role) {
        case Role::Admin:
        case Role::Family:
            result.read  = true;
            result.write = true;
            break;
        case Role::Guest:
            return result;
        }
    }

    // Rule 5. Removal is the one action that cannot be undone, so it is the most
    // narrowly granted.
    if (result.write) {
        switch (user.role) {
        case Role::Admin:
            result.remove = true;
            break;
        case Role::Family:
            // Only their own uploads. An unknown owner denies rather than allows:
            // a file with no recorded uploader predates the index, and guessing
            // in the permissive direction would let anyone delete it.
            result.remove = fileOwnerId.has_value() && *fileOwnerId == user.id;
            break;
        case Role::Guest:
            result.remove = false;
            break;
        }
    }

    return result;
}

} // namespace auth
