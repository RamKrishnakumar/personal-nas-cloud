#include "handlers/FileRouter.h"

#include "db/AuditLog.h"
#include "handlers/AuthHandler.h"
#include "http/Connection.h"
#include "http/MimeType.h"
#include "http/PathResolver.h"
#include "http/RequestParser.h"
#include "media/ThumbnailCache.h"
#include "media/VideoStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>

namespace {

/** Escapes text for inclusion in HTML element content or a quoted attribute. */
QString escapeHtml(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&':  out += QLatin1String("&amp;");  break;
        case '<':  out += QLatin1String("&lt;");   break;
        case '>':  out += QLatin1String("&gt;");   break;
        case '"':  out += QLatin1String("&quot;"); break;
        case '\'': out += QLatin1String("&#39;");  break;
        default:   out += c;                       break;
        }
    }
    return out;
}

/** Percent-encodes one path segment for use in a URL. */
QString encodeSegment(const QString& segment)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(segment));
}

using QueryParameters = QHash<QByteArray, QString>;

/**
 * @brief Parses `a=1&b=two%20words` into decoded name/value pairs.
 *
 * Names stay as bytes; values are percent-decoded, since a path parameter can
 * legitimately contain any character.
 */
QueryParameters parseQuery(QByteArrayView query)
{
    QueryParameters parameters;

    qsizetype start = 0;
    while (start <= query.size()) {
        qsizetype amp = query.indexOf('&', start);
        if (amp < 0)
            amp = query.size();

        const QByteArrayView pair = query.sliced(start, amp - start);
        start = amp + 1;

        if (pair.isEmpty())
            continue;

        const qsizetype equals = pair.indexOf('=');
        if (equals < 0) {
            parameters.insert(pair.toByteArray(), QString());
            continue;
        }

        // '+' means space in a query string, unlike in a path.
        QByteArray rawValue = pair.sliced(equals + 1).toByteArray();
        rawValue.replace('+', ' ');

        parameters.insert(pair.sliced(0, equals).toByteArray(),
                          QUrl::fromPercentEncoding(rawValue));
    }

    return parameters;
}

/** True when the query string contains download=1. */
bool wantsDownload(QByteArrayView query)
{
    qsizetype start = 0;
    while (start <= query.size()) {
        qsizetype amp = query.indexOf('&', start);
        if (amp < 0)
            amp = query.size();
        if (query.sliced(start, amp - start) == QByteArrayView("download=1"))
            return true;
        start = amp + 1;
    }
    return false;
}

/** Builds "/_thumb/<tier>/<path>?v=<tag>" for one entry. */
QString buildThumbnailUrl(const QFileInfo& entry, const QString& encodedUrl,
                          media::ThumbTier tier)
{
    // The version tag is what makes an immutable cache policy safe: it changes
    // when the source file does, so an edited photo is a different URL rather than
    // a stale entry a browser would keep for a year.
    const QByteArray tag = media::thumbnailVersionTag(
        entry.absoluteFilePath(),
        entry.lastModified().toMSecsSinceEpoch(),
        entry.size(), tier);

    return QStringLiteral("/_thumb/%1%2?v=%3")
        .arg(QLatin1Char(media::tierCode(tier)))
        .arg(encodedUrl, QString::fromLatin1(tag));
}

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    return QStringLiteral("%1.%2 GB")
        .arg(bytes / (1024LL * 1024 * 1024))
        .arg((bytes % (1024LL * 1024 * 1024)) / (1024LL * 1024 * 102));
}

} // namespace

namespace handlers {

FileRouter::FileRouter(QString nasRoot)
    : m_nasRoot(std::move(nasRoot))
{
}

void FileRouter::setNasRoot(const QString& nasRoot)
{
    m_nasRoot = nasRoot;
}

QString FileRouter::nasRelativePath(const QString& resolvedAbsolutePath) const
{
    QString relative = resolvedAbsolutePath;

    // resolveSafe() guarantees the result is inside the root, so this only has to
    // strip the prefix rather than verify containment again.
    if (relative.startsWith(m_nasRoot, Qt::CaseInsensitive))
        relative = relative.mid(m_nasRoot.size());

    return auth::normalisePermissionPath(relative);
}

void FileRouter::handle(http::Connection& connection, const http::Request& request)
{
    // Authentication endpoints stay reachable: this overload is what the transport
    // test suites use, and handlers::AuthGate routes them before reaching here in
    // production.
    if (m_authentication && AuthHandler::handles(request.path)) {
        m_authentication->handle(connection, request);
        return;
    }

    if (!m_allowAnonymous) {
        // No identity and no explicit permission to proceed without one. Refusing
        // is the whole point of the default — see setAnonymousAccess().
        connection.sendError(401);
        return;
    }

    // Stands in for a real identity in the transport suites: full rights, so those
    // tests exercise framing and ranges rather than permission resolution.
    //
    // The id must be positive. auth::resolve() treats a non-positive id as nobody,
    // which is what makes a default-constructed context deny — so a sentinel like
    // -1 would be refused here by that same guard.
    auth::AuthContext anonymous;
    anonymous.authenticated = true;
    anonymous.user.id       = 1;
    anonymous.user.role     = auth::Role::Admin;
    anonymous.user.enabled  = true;

    handle(connection, request, anonymous);
}

void FileRouter::handle(http::Connection&        connection,
                        const http::Request&     request,
                        const auth::AuthContext& context)
{
    // Checked before the method filter below, because these are the only routes
    // that accept anything other than GET and HEAD.
    if (m_authentication && AuthHandler::handles(request.path)) {
        m_authentication->handle(connection, request);
        return;
    }

    // POST is accepted on exactly one path, and the exception is named here rather
    // than inside the handler so the set of methods this router answers stays
    // readable in one place. Upload and delete arrive in v0.8, behind the permissions.
    const bool videoApiPost = request.method == "POST"
                              && request.path == QByteArrayLiteral("/api/video");

    if (request.method != "GET" && request.method != "HEAD" && !videoApiPost) {
        connection.sendError(405);
        return;
    }

    // ── Internal routes, checked before the filesystem ────────────────────
    static const QByteArray thumbPrefix  = QByteArrayLiteral("/_thumb/");
    static const QByteArray staticPrefix = QByteArrayLiteral("/_static/");

    if (request.path.startsWith(thumbPrefix)) {
        handleThumbnail(connection, request, request.path.mid(thumbPrefix.size()),
                        context);
        return;
    }

    if (request.path.startsWith(staticPrefix)) {
        handleStaticAsset(connection, request.path.mid(staticPrefix.size()));
        return;
    }

    static const QByteArray videoPrefix = QByteArrayLiteral("/_video/");

    if (request.path.startsWith(videoPrefix)) {
        handleVideoFile(connection, request, request.path.mid(videoPrefix.size()),
                        context);
        return;
    }

    if (request.path == QByteArrayLiteral("/api/video")) {
        handleVideoApi(connection, request, context);
        return;
    }

    if (request.path == QByteArrayLiteral("/api/list")) {
        handleListApi(connection, request, context);
        return;
    }

    const auto resolved = http::resolveSafe(m_nasRoot, QString::fromUtf8(request.path));
    if (!resolved) {
        connection.sendError(403);
        return;
    }

    // Checked against the resolved location, not the requested one.
    const QString relative = nasRelativePath(*resolved);
    if (!context.permissionFor(relative).read) {
        // 404 rather than 403 for a path the requester may not read. 403 would
        // confirm that the folder exists, which is exactly what a guest who is
        // probing for other people's albums is trying to learn.
        //
        // The log is the one place the distinction survives: the client is told only
        // that there is nothing there, while the owner of the machine can see that
        // someone asked for a path they have no grant on.
        if (m_audit) {
            m_audit->recordDownload(context.user.id, relative, -1,
                                    connection.peerAddress(), 404);
        }
        connection.sendError(404);
        return;
    }

    const QFileInfo info(*resolved);
    if (!info.exists()) {
        // Distinct from 403 on purpose: collapsing the two would tell a prober
        // which paths exist outside the root.
        connection.sendError(404);
        return;
    }

    if (info.isDir()) {
        // Directories serve the application shell rather than a rendered listing.
        // The client reads its own location and fetches /api/list, which keeps deep
        // links working and bookmarkable while sending no listing markup at all.
        sendApplicationShell(connection);
        return;
    }

    // Recorded before the transfer rather than after it, so the row exists even if
    // the client vanishes mid-download. `bytes` is therefore the size of the file
    // asked for, not a count of bytes delivered — a Range request or an abandoned
    // transfer moves fewer. An abandoned stream shows up in the application log,
    // which is the right place for a transport event.
    if (m_audit) {
        m_audit->recordDownload(context.user.id, relative, info.size(),
                                connection.peerAddress(), 200);
    }

    connection.sendFile(*resolved, wantsDownload(request.query));
}

void FileRouter::handleThumbnail(http::Connection& connection,
                                 const http::Request& request,
                                 const QByteArray& remainder,
                                 const auth::AuthContext& context)
{
    if (!m_thumbnails) {
        connection.sendError(404);
        return;
    }

    // "<tier>/<encoded path>"
    const qsizetype slash = remainder.indexOf('/');
    if (slash != 1) {
        connection.sendError(404);
        return;
    }

    const auto tier = media::tierFromCode(remainder.at(0));
    if (!tier) {
        connection.sendError(404);
        return;
    }

    const auto resolved =
        http::resolveSafe(m_nasRoot, QString::fromUtf8(remainder.mid(slash)));
    if (!resolved) {
        connection.sendError(403);
        return;
    }

    // A thumbnail is a readable copy of the file, so it needs the same permission
    // the file does. Without this check a guest could see every photo in the
    // library at 320 px just by guessing paths.
    if (!context.permissionFor(nasRelativePath(*resolved)).read) {
        connection.sendError(404);
        return;
    }

    if (!QFileInfo::exists(*resolved)) {
        connection.sendError(404);
        return;
    }

    // Immutable: the URL carries the source file's modification time and size, so
    // a changed file is a different URL and a browser may keep this one forever.
    // This is what makes a second visit to a folder cost almost nothing.
    static const QByteArrayView immutable = "public, max-age=31536000, immutable";

    if (const auto cached = m_thumbnails->hit(*resolved, *tier)) {
        connection.sendFile(*cached, false, immutable);
        return;
    }

    // Miss. The worker returns immediately and the response is sent from the
    // handler below, on this same thread. Blocking here would serialise an entire
    // folder of misses behind the decode queue.
    //
    // &connection as the context object means Qt drops the callback if the
    // connection dies first, which it often will.
    connection.deferResponse();

    m_thumbnails->requestAsync(*resolved, *tier, &connection,
        [&connection](bool ok, const QString& filePath) {
            if (ok) {
                connection.sendFile(filePath, false, immutable);
            } else {
                // Decode failed — an unsupported variant, or a corrupt file. The
                // grid shows a broken image rather than stalling on it.
                connection.sendError(404);
            }
        });
}


// ─────────────────────────────────────────────────────────────────────────────
//  Listing API
// ─────────────────────────────────────────────────────────────────────────────

void FileRouter::handleListApi(http::Connection& connection,
                               const http::Request& request,
                               const auth::AuthContext& context)
{
    const QueryParameters query = parseQuery(request.query);

    const QString requestedPath = query.value(QByteArrayLiteral("path"),
                                              QStringLiteral("/"));

    const auto resolved = http::resolveSafe(m_nasRoot,
                                            QUrl::toPercentEncoding(requestedPath,
                                                                    "/"));
    if (!resolved) {
        connection.sendError(403);
        return;
    }

    // The target comes from a query parameter, which is why the gate cannot check
    // it and this has to. Resolved first, so the permission applies to the folder
    // that will actually be read.
    const QString listedPath = nasRelativePath(*resolved);
    if (!context.permissionFor(listedPath).read) {
        if (m_audit) {
            m_audit->recordList(context.user.id, listedPath,
                                connection.peerAddress(), 404);
        }
        connection.sendError(404);
        return;
    }

    const QFileInfo info(*resolved);
    if (!info.exists() || !info.isDir()) {
        connection.sendError(404);
        return;
    }

    // ── Sorting ───────────────────────────────────────────────────────────
    const QByteArray sortKey = query.value(QByteArrayLiteral("sort"),
                                           QStringLiteral("name")).toUtf8();
    const bool descending = query.value(QByteArrayLiteral("order"),
                                        QStringLiteral("asc")) == QLatin1String("desc");

    QDir::SortFlags sortFlags = QDir::DirsFirst | QDir::IgnoreCase;

    // QDir's natural direction is not the same for every key: Name sorts
    // ascending, while Size and Time sort descending — largest and newest first.
    // Reversed therefore has to be applied relative to the chosen key's own
    // direction, not relative to "ascending".
    bool naturalOrderIsDescending = false;

    if (sortKey == "size") {
        sortFlags |= QDir::Size;
        naturalOrderIsDescending = true;
    } else if (sortKey == "mtime") {
        sortFlags |= QDir::Time;
        naturalOrderIsDescending = true;
    } else {
        sortFlags |= QDir::Name;
    }

    if (descending != naturalOrderIsDescending)
        sortFlags |= QDir::Reversed;

    QFileInfoList entries =
        QDir(*resolved).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, sortFlags);

    // ── Visibility ────────────────────────────────────────────────────────
    // Removed before paging rather than after. Filtering a page would leave ragged
    // pages, a total that disagrees with what is visible, and a scroller whose
    // arithmetic no longer matches the data.
    //
    // Skipped entirely when the requester holds no folder grants: then nothing can
    // be individually denied, and checking five thousand entries would be work with
    // a guaranteed answer. A guest with no grants never reaches this point, because
    // the directory check above already refused them.
    if (!context.folderPerms.isEmpty()) {
        QFileInfoList visible;
        visible.reserve(entries.size());

        for (const QFileInfo& entry : entries) {
            if (context.permissionFor(nasRelativePath(entry.absoluteFilePath())).read)
                visible.append(entry);
        }

        entries = visible;
    }

    // ── Paging ────────────────────────────────────────────────────────────
    // Bounded so one request cannot be asked to serialise an entire enormous
    // directory, which would undo the point of paging.
    const int total  = int(entries.size());
    const int offset = std::clamp(query.value(QByteArrayLiteral("offset"),
                                              QStringLiteral("0")).toInt(),
                                 0, total);
    const int limit  = std::clamp(query.value(QByteArrayLiteral("limit"),
                                              QStringLiteral("200")).toInt(),
                                 1, 1000);

    // First page only. Scrolling a five-thousand-file folder issues twenty-five of
    // these calls, and they all mean the same thing — "they opened this folder". One
    // row per visit keeps that fact without burying every other entry in the table
    // under pagination.
    if (m_audit && offset == 0) {
        m_audit->recordList(context.user.id, listedPath, connection.peerAddress(),
                            200);
    }

    const QString base = requestedPath.endsWith(QLatin1Char('/'))
                             ? requestedPath
                             : requestedPath + QLatin1Char('/');

    QJsonArray items;
    for (int index = offset; index < total && index < offset + limit; ++index) {
        const QFileInfo& entry = entries.at(index);

        // Short keys: at a few thousand entries, field names are a measurable
        // fraction of the payload.
        QJsonObject item;
        item[QStringLiteral("n")] = entry.fileName();

        if (entry.isDir()) {
            item[QStringLiteral("d")] = true;
            items.append(item);
            continue;
        }

        const QByteArray mime = http::mimeTypeForPath(entry.fileName());
        const bool isImage = mime.startsWith("image/");
        const bool isVideo = mime.startsWith("video/");

        item[QStringLiteral("s")] = entry.size();
        item[QStringLiteral("m")] = entry.lastModified().toMSecsSinceEpoch();
        item[QStringLiteral("k")] = isImage ? QStringLiteral("image")
                                   : isVideo ? QStringLiteral("video")
                                             : QStringLiteral("file");

        const QString url = base + encodeSegment(entry.fileName());
        item[QStringLiteral("u")] = url;

        // Thumbnail URLs are produced here so the client never has to derive a cache
        // key. Offered only where one can actually be produced, which canGenerateFor()
        // answers: Qt-readable images always, and videos and HEIC photos once step 17
        // put ffmpeg behind them. Without that binary those two go back to a
        // placeholder rather than to a URL guaranteed to 404.
        //
        // Also skipped for a file whose extension lies — a `.JPG` that is really a
        // QuickTime movie — once one attempt has established as much.
        // The MIME prefix is deliberately not part of this test. canGenerateFor() is
        // the authority on what can be thumbnailed, and the two disagree at the
        // edges — `.hif` is a HEIF photo ffmpeg reads happily but is absent from the
        // MIME table, so gating on `image/*` would silently exclude it.
        if (m_thumbnails
            && m_thumbnails->canGenerateFor(entry.fileName())
            && !m_thumbnails->isKnownUndecodable(
                   entry.absoluteFilePath(),
                   entry.lastModified().toMSecsSinceEpoch(), entry.size())) {
            item[QStringLiteral("t")] =
                buildThumbnailUrl(entry, url, media::ThumbTier::Small);

            // Grid tier only for a video. The overlay plays the file itself, so a
            // 1280 px poster frame would never be fetched — and generating one means
            // a second ffmpeg process per video for an image nothing displays.
            if (!isVideo) {
                item[QStringLiteral("p")] =
                    buildThumbnailUrl(entry, url, media::ThumbTier::Medium);
            }
        }

        // Where a video's optimised rendition stands, so opening one that is already
        // done needs no extra round trip. Only the states that change what the client
        // does are sent: a queued or running job is the client's business to poll for,
        // not the listing's to report, and the listing is cached by nothing.
        if (isVideo && m_videos && m_videos->isAvailable()) {
            const media::VideoStatus optimised =
                m_videos->status(entry.absoluteFilePath());

            if (optimised.state == media::VideoState::Ready) {
                // `url` is already the percent-encoded NAS-relative path and starts
                // with a slash, so prefixing is all that is needed — and it keeps
                // this in step with the route by construction.
                item[QStringLiteral("vo")] = QStringLiteral("/_video") + url;
                item[QStringLiteral("vs")] = QStringLiteral("ready");
            } else if (optimised.state != media::VideoState::Unknown) {
                item[QStringLiteral("vs")] =
                    media::videoStateToString(optimised.state);
            }
        }

        items.append(item);
    }

    QJsonObject body;
    body[QStringLiteral("path")]    = requestedPath;
    body[QStringLiteral("total")]   = total;
    body[QStringLiteral("offset")]  = offset;
    body[QStringLiteral("limit")]   = limit;
    body[QStringLiteral("entries")] = items;

    // Resolved server-side. The client uses it only to hide unavailable actions;
    // every mutating request is re-checked, so tampering with this buys nothing.
    const auth::Perm folderPermission = context.permissionFor(listedPath);

    QJsonObject permissions;
    permissions[QStringLiteral("read")]  = folderPermission.read;
    permissions[QStringLiteral("write")] = folderPermission.write;

    // Folder-level, so it is false for a family member even where they may delete
    // their own uploads: that depends on the individual file's recorded uploader,
    // which only the per-file check knows. The role is sent so the client can say
    // "your own files" rather than hiding the action entirely.
    permissions[QStringLiteral("delete")] = folderPermission.remove;
    permissions[QStringLiteral("role")]   =
        context.authenticated ? auth::roleToString(context.user.role) : QString();
    permissions[QStringLiteral("user")]   = context.user.username;

    body[QStringLiteral("perm")] = permissions;

    // Built with QJsonDocument rather than string concatenation, so a filename
    // containing a quote or a backslash cannot break the document.
    connection.send(200, "application/json",
                    QJsonDocument(body).toJson(QJsonDocument::Compact),
                    "Cache-Control: no-store\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static assets
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  Optimised video
// ─────────────────────────────────────────────────────────────────────────────

void FileRouter::handleVideoApi(http::Connection& connection,
                                const http::Request& request,
                                const auth::AuthContext& context)
{
    const QueryParameters query = parseQuery(request.query);
    const QString requested = query.value(QByteArrayLiteral("path"), QString());

    const auto resolved =
        http::resolveSafe(m_nasRoot, QUrl::toPercentEncoding(requested, "/"));
    if (!resolved) {
        connection.sendError(403);
        return;
    }

    // Against the resolved path, by the handler that resolved it — the same rule as
    // every other route here, and for the same reason.
    const QString relative = nasRelativePath(*resolved);
    if (!context.permissionFor(relative).read) {
        connection.sendError(404);
        return;
    }

    QJsonObject body;
    body[QStringLiteral("path")] = relative;

    if (!m_videos || !m_videos->isAvailable()) {
        // Explicitly "unavailable" rather than an error. The client's correct
        // response is to play the original, which is what it did before any of this
        // existed, and an error would make it look broken instead.
        body[QStringLiteral("state")]     = QStringLiteral("unavailable");
        body[QStringLiteral("available")] = false;
        connection.send(200, "application/json; charset=utf-8",
                        QJsonDocument(body).toJson(QJsonDocument::Compact),
                        "Cache-Control: no-store\r\n");
        return;
    }

    // POST asks for the work; GET only reports. A GET that started a four-minute
    // transcode would let a link prefetcher set a whole library encoding.
    const media::VideoStatus status = request.method == "POST"
                                          ? m_videos->request(*resolved)
                                          : m_videos->status(*resolved);

    body[QStringLiteral("available")] = true;
    body[QStringLiteral("state")]     = media::videoStateToString(status.state);

    if (status.state == media::VideoState::Running)
        body[QStringLiteral("percent")] = status.percent;

    if (status.state == media::VideoState::Queued)
        body[QStringLiteral("ahead")] = status.queueAhead;

    if (!status.reason.isEmpty())
        body[QStringLiteral("reason")] = status.reason;

    if (status.state == media::VideoState::Ready) {
        body[QStringLiteral("url")] = QStringLiteral("/_video/")
                                      + QString::fromUtf8(
                                            QUrl::toPercentEncoding(relative, "/"));
    }

    connection.send(200, "application/json; charset=utf-8",
                    QJsonDocument(body).toJson(QJsonDocument::Compact),
                    "Cache-Control: no-store\r\n");
}

void FileRouter::handleVideoFile(http::Connection& connection,
                                 const http::Request& request,
                                 const QByteArray& remainder,
                                 const auth::AuthContext& context)
{
    Q_UNUSED(request)

    if (!m_videos) {
        connection.sendError(404);
        return;
    }

    // The URL names the *source*, not the rendition, and the store derives the
    // rendition from it. Naming the cache key instead would mean the client could
    // ask for a file by hash, which is a path the permission check cannot see.
    const auto resolved = http::resolveSafe(m_nasRoot, remainder);
    if (!resolved) {
        connection.sendError(403);
        return;
    }

    if (!context.permissionFor(nasRelativePath(*resolved)).read) {
        connection.sendError(404);
        return;
    }

    const auto rendition = m_videos->readyPath(*resolved);
    if (!rendition) {
        connection.sendError(404);
        return;
    }

    // Immutable, like a thumbnail and for the same reason: the cache key embeds the
    // source's modification time and size, so a changed file produces a different
    // rendition. This is also what lets a browser keep the ~10 MB it downloaded,
    // which is the whole reason replay becomes instant.
    connection.sendFile(*rendition, /*forceDownload=*/false,
                        "private, max-age=31536000, immutable");
}

void FileRouter::handleStaticAsset(http::Connection& connection,
                                   const QByteArray& assetName)
{
    // A path separator or parent reference here would let a request reach outside
    // the resource prefix, so only plain names are accepted.
    const QString name = QString::fromUtf8(assetName);
    if (name.isEmpty() || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\')) || name.contains(QLatin1String(".."))) {
        connection.sendError(404);
        return;
    }

    QFile asset(QStringLiteral(":/web/") + name);
    if (!asset.open(QIODevice::ReadOnly)) {
        connection.sendError(404);
        return;
    }

    // Revalidated rather than cached for a long period: without a bundler the
    // filenames carry no content hash, so a stale copy would survive an upgrade.
    // These are small and gzipped, making the conditional request cheap.
    connection.send(200, http::mimeTypeForPath(name), asset.readAll(),
                    "Cache-Control: no-cache\r\n");
}

void FileRouter::sendApplicationShell(http::Connection& connection)
{
    QFile shell(QStringLiteral(":/web/index.html"));
    if (!shell.open(QIODevice::ReadOnly)) {
        // Only possible if the resources failed to link, which would otherwise
        // present as a blank page with no explanation.
        connection.send(500, "text/plain; charset=utf-8",
                        "Web interface assets are missing from this build.");
        return;
    }

    connection.send(200, "text/html; charset=utf-8", shell.readAll(),
                    "Cache-Control: no-cache\r\n");
}

} // namespace handlers
