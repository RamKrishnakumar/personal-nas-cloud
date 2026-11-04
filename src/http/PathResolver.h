#pragma once

#include <QString>
#include <optional>

namespace http {

/**
 * @brief Maps a URL path onto a filesystem path, refusing anything that escapes
 *        the NAS root.
 *
 * @param nasRoot        Absolute path to the directory being served. Must exist.
 * @param encodedUrlPath The raw, still percent-encoded path from the request
 *                       line, for example `/Photos/IMG%200421.jpg`. Any query
 *                       string must already be stripped.
 *
 * @return The absolute filesystem path, canonicalised when the target exists, or
 *         @c std::nullopt when the request must be answered with 403.
 *
 * Returning a path does not mean the file exists — a non-existent but otherwise
 * legitimate path is returned so the caller can answer 404. Only an escape
 * attempt, a malformed path, or an unreadable root produces @c nullopt.
 *
 * ### What this rejects, and why each check is needed
 *
 * | Check | Attack it stops |
 * |---|---|
 * | Any `..` segment | `/../../Windows/System32/config/SAM` |
 * | NUL and control characters | Truncation tricks against C APIs below Qt |
 * | `:` inside a segment (Windows) | `file.txt:hidden` NTFS alternate data streams |
 * | Reserved device names (Windows) | `CON`, `NUL`, `COM1` — these open devices, not files |
 * | Trailing dots or spaces (Windows) | `secret.txt.` and `secret.txt ` resolve to `secret.txt`, bypassing an extension check |
 * | Lexical prefix test with a separator | `E:/Foobar` escaping a root of `E:/Foo` |
 * | Canonical path re-test | Symlinks and NTFS junctions pointing outside the root |
 *
 * The cheap string checks run first so the expensive `canonicalFilePath()`
 * syscall only happens for paths that already look legitimate.
 *
 * Path comparison is case-insensitive on Windows and macOS, case-sensitive on
 * Linux, matching the behaviour of the underlying filesystem. Getting this wrong
 * in either direction is a bug: too strict and valid requests 403, too loose and
 * the prefix test can be defeated by changing case.
 *
 * @note `..` segments are rejected outright rather than collapsed. A file server
 *       has no legitimate use for them, and refusing is easier to reason about
 *       than normalising.
 */
std::optional<QString> resolveSafe(const QString& nasRoot,
                                   const QString& encodedUrlPath);

/**
 * @brief Turns an absolute path inside the library back into a library-relative one.
 *
 * @param configuredRoot The root as the owner wrote it, which may be a symlink.
 * @param canonicalRoot  The same directory with every link resolved. May be empty
 *                       when the root does not currently exist.
 * @param absolutePath   A path already known to be inside the root.
 *
 * @return The part below the root, beginning with `/`, or the input unchanged if it
 *         matches neither root.
 *
 * ### Why both roots have to be passed
 *
 * resolveSafe() does not return paths in one namespace. When the target exists it
 * returns the *canonical* path; when it does not, it returns one built from the
 * configured root. Those are the same string only when the root contains no links.
 *
 * On macOS they are never the same for a temporary directory, because `/var` is a
 * symlink to `/private/var`. That is how this was found: stripping a configured root
 * of `/var/folders/.../nas` off a canonical `/private/var/folders/.../nas/Private`
 * matches nothing, so the whole absolute path survived.
 *
 * It is not a macOS quirk, only a place where it always happens. Any owner whose
 * library path goes through a symlink — `~/Library` pointing at an external volume,
 * `/home` symlinked on some Linux distributions — reaches the same state.
 *
 * ### Why it matters more than a cosmetic path
 *
 * The result is what folder permissions are matched against. Left unstripped, a rule
 * written for `/Private` no longer covers the request, so auth::resolve() finds no
 * matching prefix and falls through to its role default — which, for the Family and
 * Admin roles, grants read and write. **An explicit denial on a subfolder would be
 * silently bypassed.** Guests fail closed, so the damage is confined to the roles
 * that already have broad access, but a deny rule that stops applying is still a
 * deny rule that stopped applying.
 *
 * Kept pure, and taking both roots as strings rather than reading the filesystem, so
 * that every case above is testable on a machine whose temporary directory is not
 * symlinked.
 */
QString stripRoot(const QString& configuredRoot,
                  const QString& canonicalRoot,
                  const QString& absolutePath);

} // namespace http
