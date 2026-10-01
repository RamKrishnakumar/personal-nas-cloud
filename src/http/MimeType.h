#pragma once

#include <QByteArray>
#include <QString>

namespace http {

/**
 * @brief Content type for @p path, decided from its extension alone.
 *
 * Extension-only on purpose. QMimeDatabase's default mode opens the file and
 * sniffs its contents, and v0.5 called it twice for every directory entry — about
 * a thousand file opens before the first byte of HTML left the server for a
 * 500-file folder. On a spinning disk that alone accounted for seconds of delay.
 *
 * Sniffing buys nothing here: the files are the user's own, the type only selects
 * a Content-Type header, and a wrong guess from a wrong extension is the user's
 * own mislabelling rather than a security boundary.
 *
 * @return A MIME type, or `application/octet-stream` for anything unrecognised.
 */
QByteArray mimeTypeForPath(const QString& path);

/** True for types the browser can display inline, i.e. images and video. */
bool isDisplayableMedia(QByteArrayView mimeType);

} // namespace http
