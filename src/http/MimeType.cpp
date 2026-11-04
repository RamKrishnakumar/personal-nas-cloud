#include "http/MimeType.h"

#include <QFileInfo>
#include <QHash>

namespace {

/**
 * Extension to content type. Deliberately short: it covers what a personal photo
 * and video library actually holds, plus what the web UI serves. Anything else
 * downloads as application/octet-stream, which is correct rather than merely
 * tolerable.
 */
const QHash<QString, QByteArray>& mimeTable()
{
    static const QHash<QString, QByteArray> table = {
        // Images
        { QStringLiteral("jpg"),  "image/jpeg" },
        { QStringLiteral("jpeg"), "image/jpeg" },
        { QStringLiteral("png"),  "image/png" },
        { QStringLiteral("gif"),  "image/gif" },
        { QStringLiteral("webp"), "image/webp" },
        { QStringLiteral("avif"), "image/avif" },
        { QStringLiteral("bmp"),  "image/bmp" },
        { QStringLiteral("svg"),  "image/svg+xml" },
        { QStringLiteral("heic"), "image/heic" },
        { QStringLiteral("heif"), "image/heif" },
        { QStringLiteral("tif"),  "image/tiff" },
        { QStringLiteral("tiff"), "image/tiff" },

        // Video
        { QStringLiteral("mp4"),  "video/mp4" },
        { QStringLiteral("m4v"),  "video/mp4" },
        { QStringLiteral("mov"),  "video/quicktime" },
        { QStringLiteral("webm"), "video/webm" },
        { QStringLiteral("mkv"),  "video/x-matroska" },
        { QStringLiteral("avi"),  "video/x-msvideo" },
        { QStringLiteral("mpg"),  "video/mpeg" },
        { QStringLiteral("mpeg"), "video/mpeg" },
        { QStringLiteral("3gp"),  "video/3gpp" },

        // Audio
        { QStringLiteral("mp3"),  "audio/mpeg" },
        { QStringLiteral("m4a"),  "audio/mp4" },
        { QStringLiteral("aac"),  "audio/aac" },
        { QStringLiteral("flac"), "audio/flac" },
        { QStringLiteral("ogg"),  "audio/ogg" },
        { QStringLiteral("opus"), "audio/opus" },
        { QStringLiteral("wav"),  "audio/wav" },

        // Documents and text
        { QStringLiteral("pdf"),  "application/pdf" },
        { QStringLiteral("txt"),  "text/plain; charset=utf-8" },
        { QStringLiteral("md"),   "text/plain; charset=utf-8" },
        { QStringLiteral("log"),  "text/plain; charset=utf-8" },
        { QStringLiteral("csv"),  "text/csv; charset=utf-8" },
        { QStringLiteral("json"), "application/json" },
        { QStringLiteral("xml"),  "application/xml" },

        // Web assets, for the SPA added in step 11
        { QStringLiteral("html"), "text/html; charset=utf-8" },
        { QStringLiteral("htm"),  "text/html; charset=utf-8" },
        { QStringLiteral("css"),  "text/css; charset=utf-8" },
        { QStringLiteral("js"),   "text/javascript; charset=utf-8" },
        { QStringLiteral("mjs"),  "text/javascript; charset=utf-8" },
        { QStringLiteral("ico"),  "image/vnd.microsoft.icon" },
        { QStringLiteral("woff2"),"font/woff2" },

        // Archives
        { QStringLiteral("zip"),  "application/zip" },
        { QStringLiteral("7z"),   "application/x-7z-compressed" },
        { QStringLiteral("gz"),   "application/gzip" },
    };
    return table;
}

} // namespace

namespace http {

QByteArray mimeTypeForPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return mimeTable().value(suffix, QByteArrayLiteral("application/octet-stream"));
}

bool isDisplayableMedia(QByteArrayView mimeType)
{
    return mimeType.startsWith("image/") || mimeType.startsWith("video/");
}

} // namespace http
