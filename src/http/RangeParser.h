#pragma once

#include <QByteArrayView>
#include <QtGlobal>

namespace http {

/**
 * @brief The outcome of parsing one HTTP Range header value.
 *
 * @c start and @c end are both inclusive, matching the Content-Range header
 * wire format (`bytes START-END/SIZE`).
 */
struct ByteRange
{
    qint64 start = 0;
    qint64 end   = 0;          ///< Inclusive last byte.
    bool   present     = false; ///< A usable Range header was supplied.
    bool   satisfiable = false; ///< The requested range lies inside the file.

    /** Number of bytes to send. Zero unless the range is satisfiable. */
    qint64 length() const { return satisfiable ? end - start + 1 : 0; }
};

/**
 * @brief Parses an HTTP/1.1 Range header value against a known file size.
 *
 * @param headerValue The text after `Range:`, for example `bytes=0-1023`.
 *                    Pass an empty view when the request had no Range header.
 * @param fileSize    Size of the target file in bytes.
 *
 * Supported forms:
 *   - `bytes=a-b`      explicit range; @c end is clamped to the last byte
 *   - `bytes=a-`       open ended; runs to the last byte
 *   - `bytes=-n`       suffix range; the final @c n bytes
 *   - `bytes=a-b,c-d`  only the first range is honoured (see note below)
 *
 * Three distinct outcomes, which callers must handle differently:
 *
 *   | Result                              | Correct response |
 *   |-------------------------------------|------------------|
 *   | `present == false`                  | 200, whole file  |
 *   | `present && !satisfiable`           | 416 + `Content-Range: bytes * / SIZE` |
 *   | `present && satisfiable`            | 206 + `Content-Range` + exact length |
 *
 * A syntactically invalid value yields `present == false` rather than an error.
 * RFC 9110 §14.2 requires an unparsable Range header to be ignored, not
 * rejected. A well-formed but out-of-bounds range is the only case that
 * produces 416.
 *
 * @note Multi-range requests return only the first range. Replying with a
 *       single 206 rather than a `multipart/byteranges` body is permitted, and
 *       media players in practice request one range at a time. Serving the
 *       first range beats ignoring the header, which would send the entire file.
 */
ByteRange parseRange(QByteArrayView headerValue, qint64 fileSize);

} // namespace http
