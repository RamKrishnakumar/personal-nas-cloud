#pragma once

#include <QByteArray>
#include <QByteArrayView>

namespace http {

/**
 * @brief Bodies smaller than this are sent uncompressed.
 *
 * Below roughly this size the gzip header and trailer, eighteen bytes before any
 * payload, cancel out the saving — and a short, high-entropy body can come out
 * larger than it went in.
 */
constexpr qsizetype MinimumCompressibleBytes = 512;

/**
 * @brief True when @p acceptEncoding permits gzip.
 *
 * Handles the comma-separated list with quality values, so `gzip;q=0` is a
 * refusal rather than a match, and a bare `*` is an acceptance.
 */
bool acceptsGzip(QByteArrayView acceptEncoding);

/**
 * @brief True for content types where compression pays for itself.
 *
 * An allowlist rather than a blocklist. JPEG, WebP, MP4 and ZIP are already
 * compressed: running deflate over them burns CPU on the reference hardware to
 * produce something marginally *larger*. HTML and JSON are the opposite — the
 * repetitive markup of a directory listing is the single most compressible thing
 * this server sends.
 */
bool isCompressibleContentType(QByteArrayView contentType);

/**
 * @brief Compresses @p data into a gzip stream (RFC 1952).
 *
 * Built on Qt's qCompress rather than on zlib directly. zlib is reachable here
 * only through Qt's private QtZlib headers or a per-toolchain system copy, and
 * neither is portable across the compilers and platforms this ships on.
 * qCompress always exists, so the gzip framing is assembled around its output.
 *
 * gzip rather than deflate because every client handles it. `Content-Encoding:
 * deflate` is specified as the zlib format, which is exactly what qCompress
 * produces, but enough clients historically mishandled it that it is not worth
 * the risk.
 *
 * @return The gzip stream, or an empty array if compression failed or @p data was
 *         itself empty. Callers must fall back to sending the body uncompressed
 *         rather than treating an empty result as valid output.
 */
QByteArray gzipCompress(const QByteArray& data, int compressionLevel = 6);

/**
 * @brief CRC-32 as used by gzip (IEEE 802.3, reflected, polynomial 0xEDB88320).
 *
 * Exposed so tests can check it against published vectors. A silently wrong CRC
 * produces a stream that every client rejects, which is hard to attribute from
 * the outside and trivial to catch here.
 */
quint32 gzipCrc32(QByteArrayView data);

} // namespace http
