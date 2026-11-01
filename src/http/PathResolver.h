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

} // namespace http
