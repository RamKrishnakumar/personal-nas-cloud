#include "http/PathResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>

#include <algorithm>

namespace {

/**
 * Filesystems differ on case sensitivity, and the prefix test below must match
 * the filesystem's own behaviour. On Windows and macOS a case-sensitive test
 * would reject `e:/photos` against a root of `E:/Photos`; on Linux a
 * case-insensitive test would let `/MNT/Storage` past a root of `/mnt/storage`
 * that it does not actually belong to.
 */
constexpr Qt::CaseSensitivity pathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

/**
 * @brief True when @p path is @p root itself or lies beneath it.
 *
 * The separator is appended before the prefix test. Without it, a root of
 * `E:/Foo` would accept the sibling directory `E:/Foobar`, which is the bug this
 * function exists to prevent.
 */
bool isWithin(const QString& root, const QString& path)
{
    const Qt::CaseSensitivity cs = pathCaseSensitivity();

    if (path.compare(root, cs) == 0)
        return true;

    QString prefix = root;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');

    return path.startsWith(prefix, cs);
}

/**
 * @brief Returns the destination of a link the filesystem will follow, or an
 *        empty string when @p info is not such a link.
 *
 * Only links the OS transparently follows count. A Windows `.lnk` shortcut is
 * deliberately excluded: it is an ordinary file whose bytes happen to describe a
 * target, so serving it discloses nothing and refusing it would 403 a legitimate
 * file. QFileInfo::isSymLink() lumps shortcuts in with real links, which is why
 * the narrower isSymbolicLink() and isJunction() are used here.
 */
QString followableLinkTarget(const QFileInfo& info)
{
    if (info.isSymbolicLink())
        return info.symLinkTarget();

#if defined(Q_OS_WIN)
    if (info.isJunction())
        return info.junctionTarget();
#endif

    return QString();
}

/** Rejects NUL and the C0/C1 control ranges. */
bool hasControlCharacter(const QString& text)
{
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F))
            return true;
    }
    return false;
}

#if defined(Q_OS_WIN)
/**
 * @brief True for names Windows resolves to a device rather than a file.
 *
 * The check applies to the part before the first dot, because `CON.txt` opens
 * the console just as `CON` does. Opening one of these from a file server hangs
 * the handling thread or returns device data.
 */
bool isReservedDeviceName(const QString& segment)
{
    static const QStringList reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"), QStringLiteral("COM9"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"),
        QStringLiteral("LPT7"), QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };

    const QString stem = segment.section(QLatin1Char('.'), 0, 0);
    return reserved.contains(stem, Qt::CaseInsensitive);
}
#endif

/** Per-segment validation. Returns false when the segment must be refused. */
bool isSegmentSafe(const QString& segment)
{
    if (segment == QLatin1String(".."))
        return false;

#if defined(Q_OS_WIN)
    // A colon anywhere in a segment names an NTFS alternate data stream.
    if (segment.contains(QLatin1Char(':')))
        return false;

    // Windows silently strips trailing dots and spaces, so `secret.txt.` and
    // `secret.txt ` both open `secret.txt`. Refuse rather than normalise.
    if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' ')))
        return false;

    if (isReservedDeviceName(segment))
        return false;
#endif

    return true;
}

} // namespace

namespace http {

std::optional<QString> resolveSafe(const QString& nasRoot,
                                   const QString& encodedUrlPath)
{
    // ── Establish the root ────────────────────────────────────────────────
    // Canonicalise once, so a root that is itself a symlink still matches the
    // canonical form of its children in the final check below.
    const QFileInfo rootInfo(nasRoot);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty())
        return std::nullopt;                 // Root missing or unreadable.

    const QString rootClean = QDir::cleanPath(rootInfo.absoluteFilePath());

    // ── Decode exactly once ───────────────────────────────────────────────
    // Decoding once and then refusing every ".." segment is what makes encoded
    // traversal (%2e%2e, %2E%2E) harmless: after one decode it is a literal
    // ".." and gets rejected below. We deliberately do not attempt to detect
    // double encoding, because nothing downstream decodes a second time, and
    // such a check would reject legitimate filenames containing a percent sign.
    const QString decoded = QUrl::fromPercentEncoding(encodedUrlPath.toUtf8());

    if (hasControlCharacter(decoded))
        return std::nullopt;

    // ── Validate and rebuild the path segment by segment ──────────────────
    // Backslashes are folded to forward slashes before splitting. A backslash is
    // a separator on Windows, so treating it as one everywhere stops
    // `..\..\Windows` from surviving as a single segment that passes the ".."
    // test and is then split by the OS. Folding beats a regex split here: it
    // runs on every request.
    QString normalised = decoded;
    normalised.replace(QLatin1Char('\\'), QLatin1Char('/'));

    QStringList safeSegments;
    const QStringList rawSegments =
        normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    for (const QString& segment : rawSegments) {
        if (segment == QLatin1String("."))
            continue;

        if (!isSegmentSafe(segment))
            return std::nullopt;

        safeSegments.append(segment);
    }

    // ── Assemble and verify lexically ─────────────────────────────────────
    QString candidate = rootClean;
    if (!safeSegments.isEmpty())
        candidate += QLatin1Char('/') + safeSegments.join(QLatin1Char('/'));

    candidate = QDir::cleanPath(candidate);

    if (!isWithin(rootClean, candidate))
        return std::nullopt;

    // ── Verify against the real filesystem ────────────────────────────────
    // Only reachable for paths that already look valid, so the syscall cost is
    // paid on legitimate requests rather than on every probe.
    //
    // Walk the segments and resolve every link encountered, checking containment
    // at each step. canonicalFilePath() alone is NOT sufficient: on Windows it
    // does not resolve an NTFS directory junction, and QFileInfo reports
    // isSymLink() == false for one. A junction planted inside the served folder
    // would therefore pass a canonical-path test unnoticed. Walking also catches
    // a link part-way along a path, which a single check on the leaf misses on
    // every platform.
    QString walked = rootCanonical;
    for (const QString& segment : safeSegments) {
        walked += QLatin1Char('/') + segment;

        const QFileInfo step(walked);
        if (!step.exists())
            break;                           // Nothing further can be a link.

        const QString linkTarget = followableLinkTarget(step);
        if (linkTarget.isEmpty())
            continue;

        const QString targetCanonical = QFileInfo(linkTarget).canonicalFilePath();
        if (targetCanonical.isEmpty())
            return std::nullopt;             // Dangling link.

        if (!isWithin(rootCanonical, targetCanonical))
            return std::nullopt;             // Link escapes the root.

        walked = targetCanonical;            // Continue from the destination.
    }

    const QFileInfo candidateInfo(candidate);
    if (!candidateInfo.exists())
        return candidate;                    // Caller answers 404.

    // Second, independent layer. This is the one that works for POSIX symlinks
    // and costs one syscall, so it stays even though the walk above subsumes it
    // in most cases.
    const QString canonical = candidateInfo.canonicalFilePath();
    if (canonical.isEmpty())
        return std::nullopt;                 // Broken link, or raced with a delete.

    if (!isWithin(rootCanonical, canonical))
        return std::nullopt;

    return canonical;
}

QString stripRoot(const QString& configuredRoot,
                  const QString& canonicalRoot,
                  const QString& absolutePath)
{
    // Longest first. One root can be a prefix of the other — a configured `/data`
    // whose canonical form is `/data/volumes/disk1` is an ordinary mount layout —
    // and stripping the shorter one there would leave `/volumes/disk1/...` in the
    // path, which is neither root-relative nor an absolute path, and would quietly
    // become its own permission prefix.
    //
    // Both are cleaned but neither is made absolute. Resolving a relative root here
    // would reach for the filesystem, which this function promises not to do, and on
    // Windows would silently prepend the current drive — turning a root of `/srv/nas`
    // into `C:/srv/nas` and matching nothing. Callers pass absolute roots.
    QStringList roots;
    if (!canonicalRoot.isEmpty())
        roots.append(QDir::cleanPath(canonicalRoot));
    if (!configuredRoot.isEmpty())
        roots.append(QDir::cleanPath(configuredRoot));

    std::sort(roots.begin(), roots.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });

    for (const QString& root : roots) {
        if (!isWithin(root, absolutePath))
            continue;

        QString remainder = absolutePath.mid(root.size());

        // isWithin() accepts the root itself, where the remainder is empty and the
        // library root is what was asked for.
        if (remainder.isEmpty())
            return QStringLiteral("/");

        // A root written with a trailing separator — `E:/` is the common one — has
        // already consumed the slash that should start the remainder.
        if (!remainder.startsWith(QLatin1Char('/')))
            remainder.prepend(QLatin1Char('/'));

        return remainder;
    }

    // Neither root matched. Returned unchanged rather than guessed at: the callers
    // normalise this into a permission path, and inventing a shorter one here would
    // mean inventing which folder's rules apply.
    return absolutePath;
}

} // namespace http
