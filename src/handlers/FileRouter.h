#pragma once

#include <QString>

#include "http/Router.h"

namespace media { class ThumbnailCache; }

namespace handlers {

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

    void handle(http::Connection& connection, const http::Request& request) override;

private:
    /** Serves /_thumb/<tier>/<path>, generating on demand. */
    void handleThumbnail(http::Connection& connection,
                         const http::Request& request,
                         const QByteArray& remainder);

    QString buildListingHtml(const QString& directoryPath,
                             const QString& urlPath) const;

    QString                m_nasRoot;
    media::ThumbnailCache* m_thumbnails = nullptr;
};

} // namespace handlers
