#pragma once

#include <QString>

#include "http/Router.h"

namespace media { class ThumbnailCache; }

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

    void handle(http::Connection& connection, const http::Request& request) override;

private:
    /** Serves /_thumb/<tier>/<path>, generating on demand. */
    void handleThumbnail(http::Connection& connection,
                         const http::Request& request,
                         const QByteArray& remainder);

    /**
     * @brief Serves /api/list — one page of a directory, as JSON.
     *
     * Replaces server-rendered HTML. A 5000-file folder is about 1.5 MB of markup
     * and 5000 DOM nodes for the browser to build; the same listing is roughly
     * 300 KB of JSON, around 60 KB gzipped, and the client renders only the rows
     * on screen.
     */
    void handleListApi(http::Connection& connection, const http::Request& request);

    /** Serves the single-page application shell and its assets from Qt resources. */
    void handleStaticAsset(http::Connection& connection, const QByteArray& assetName);
    void sendApplicationShell(http::Connection& connection);

    QString                m_nasRoot;
    media::ThumbnailCache* m_thumbnails     = nullptr;
    AuthHandler*           m_authentication = nullptr;
};

} // namespace handlers
