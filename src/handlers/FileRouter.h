#pragma once

#include <QString>

#include "auth/Permissions.h"
#include "http/Router.h"

namespace media { class ThumbnailCache; class VideoStore; }
namespace db { class AuditLog; }

namespace handlers {

class AuthHandler;

/**
 * @class FileRouter
 * @brief Serves directory listings and file downloads from the NAS root.
 *
 * Thread-safe by being immutable once serving starts: the root is set before
 * HttpServer::start() and only read afterwards, so no locking is needed even
 * though every worker thread calls handle() concurrently.
 *
 * This is the whole request surface for v0.6 step 7. Step 11 replaces the HTML
 * listing with a JSON API plus a static SPA, and step 14 wraps this router in
 * the authentication gate.
 */
class FileRouter : public http::Router
{
public:
    explicit FileRouter(QString nasRoot);

    /** Must not be called while the server is listening. */
    void setNasRoot(const QString& nasRoot);
    QString nasRoot() const { return m_nasRoot; }

    /**
     * @brief Supplies the thumbnail cache that backs /_thumb/.
     *
     * Optional. Without it the grid falls back to linking originals, which is
     * correct but slow — the behaviour before step 10.
     */
    void setThumbnailCache(media::ThumbnailCache* cache) { m_thumbnails = cache; }

    /**
     * @brief Supplies the login, logout and session endpoints.
     *
     * Optional in step 13: without it those paths 404 and nothing else changes.
     * Step 14 turns this into the gate every other path has to pass, at which
     * point its absence has to mean "refuse everything" rather than "allow
     * everything" — the current behaviour is only acceptable because no route is
     * protected yet.
     */
    void setAuthHandler(AuthHandler* authentication) { m_authentication = authentication; }

    /**
     * @brief Allows requests that carry no identity.
     *
     * Off by default, so a FileRouter reached without handlers::AuthGate in front
     * of it serves nothing. That default is the point: a wiring mistake produces a
     * server that refuses everything, which is noticed immediately, rather than one
     * that serves everything, which is not noticed at all.
     *
     * Turned on by the transport test suites, which exercise framing, ranges and
     * admission rather than authorisation. It has no production caller.
     */
    void setAnonymousAccess(bool allowed) { m_allowAnonymous = allowed; }

    /**
     * @brief Records downloads, folder opens and refusals.
     *
     * Optional. Thumbnails are never recorded however this is set — see db::AuditLog
     * for why that would drown everything else.
     */
    void setAuditLog(db::AuditLog* audit) { m_audit = audit; }

    /**
     * @brief Enables optimised video renditions.
     *
     * Optional. Without it `/api/video` reports that nothing is available and the
     * client plays originals, which is the behaviour before this existed.
     */
    void setVideoStore(media::VideoStore* videos) { m_videos = videos; }

    /**
     * @brief Router entry point. Denies unless setAnonymousAccess() was given.
     *
     * In production handlers::AuthGate calls the overload below instead, with a
     * context it alone can build.
     */
    void handle(http::Connection& connection, const http::Request& request) override;

    /** Serves @p request as @p context's owner. */
    void handle(http::Connection&      connection,
                const http::Request&   request,
                const auth::AuthContext& context);

private:
    /**
     * @brief NAS-relative form of an absolute path already resolved by this router.
     *
     * Permissions are checked against this rather than against the requested URL,
     * so an encoding trick or a symbolic link cannot make the check and the read
     * disagree about which folder is involved.
     */
    QString nasRelativePath(const QString& resolvedAbsolutePath) const;

    /** Serves /_thumb/<tier>/<path>, generating on demand. */
    void handleThumbnail(http::Connection& connection,
                         const http::Request& request,
                         const QByteArray& remainder,
                         const auth::AuthContext& context);

    /**
     * @brief Serves /api/list — one page of a directory, as JSON.
     *
     * Replaces server-rendered HTML. A 5000-file folder is about 1.5 MB of markup
     * and 5000 DOM nodes for the browser to build; the same listing is roughly
     * 300 KB of JSON, around 60 KB gzipped, and the client renders only the rows
     * on screen.
     */
    void handleListApi(http::Connection& connection, const http::Request& request,
                       const auth::AuthContext& context);

    /**
     * @brief Serves `/api/video` — where a video's optimised rendition stands.
     *
     * GET reports; POST asks for one to be made. Split that way because making one
     * occupies the machine for minutes, which is not something a GET should ever
     * start — a browser prefetching links would otherwise set a library transcoding.
     */
    void handleVideoApi(http::Connection& connection, const http::Request& request,
                        const auth::AuthContext& context);

    /** Serves `/_video/<encoded path>` — the rendition itself. */
    void handleVideoFile(http::Connection& connection, const http::Request& request,
                         const QByteArray& remainder,
                         const auth::AuthContext& context);

    /** Serves the single-page application shell and its assets from Qt resources. */
    void handleStaticAsset(http::Connection& connection, const QByteArray& assetName);
    void sendApplicationShell(http::Connection& connection);

    QString                m_nasRoot;

    /// The same directory with links resolved. Empty when the root does not exist.
    QString                m_nasRootCanonical;

    media::ThumbnailCache* m_thumbnails     = nullptr;
    media::VideoStore*     m_videos         = nullptr;
    AuthHandler*           m_authentication = nullptr;
    db::AuditLog*          m_audit          = nullptr;
    bool                   m_allowAnonymous = false;
};

} // namespace handlers
