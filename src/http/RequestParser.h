#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>

namespace http {

/**
 * @brief One parsed HTTP request head.
 *
 * Byte arrays rather than QString throughout: header bytes are not necessarily
 * valid UTF-8, and decoding them before validation would lose information the
 * checks below depend on. The target stays percent-encoded — decoding is
 * PathResolver's job, which needs the raw form to reason about traversal.
 */
struct Request
{
    QByteArray method;                                  ///< Upper case, e.g. "GET".
    QByteArray target;                                  ///< Raw request target, still encoded.
    QByteArray path;                                    ///< @c target up to '?', still encoded.
    QByteArray query;                                   ///< @c target after '?', may be empty.
    int        minorVersion  = 1;                       ///< 0 for HTTP/1.0, 1 for HTTP/1.1.
    qint64     contentLength = 0;                       ///< 0 when absent.
    bool       keepAlive     = true;                    ///< Resolved from version + Connection.

    /// Header names are lower-cased; order and duplicates are preserved.
    QList<std::pair<QByteArray, QByteArray>> headers;

    /**
     * @brief First value for @p lowercaseName, or an empty view when absent.
     * @param lowercaseName Must already be lower case; names are stored folded.
     */
    QByteArrayView header(QByteArrayView lowercaseName) const;

    /** Number of times @p lowercaseName appears. */
    int headerCount(QByteArrayView lowercaseName) const;
};

/**
 * @brief Outcome of one parse attempt.
 *
 * @c Incomplete is not an error — it means the caller should read more bytes and
 * try again with the longer buffer. Every other non-Complete value maps onto a
 * specific status code, listed so the router does not have to invent one.
 */
enum class ParseStatus
{
    Incomplete,           ///< Need more bytes; no response yet.
    Complete,             ///< Request head fully parsed.
    BadRequest,           ///< 400
    UriTooLong,           ///< 414
    HeadersTooLarge,      ///< 431
    NotImplemented,       ///< 501 — e.g. Transfer-Encoding, which v0.6 cannot decode.
    VersionNotSupported,  ///< 505
};

struct ParseResult
{
    ParseStatus status   = ParseStatus::Incomplete;
    qsizetype   consumed = 0;       ///< Bytes forming the head, including the blank line.
    Request     request;
};

/**
 * @brief Largest request line (method, target and version) accepted.
 *
 * Exposed so Connection, the router and the tests share one definition rather
 * than three that can drift apart.
 */
constexpr qsizetype MaxRequestLineBytes = 8 * 1024;

/** Largest header block accepted, measured up to and including the blank line. */
constexpr qsizetype MaxHeaderBlockBytes = 16 * 1024;

/**
 * @brief Parses an HTTP request head from the front of @p buffer.
 *
 * Designed to be called repeatedly on a growing buffer. A request split across
 * several socket reads yields @c Incomplete until the terminating blank line
 * arrives; nothing is retained between calls, so the caller owns the buffer and
 * simply calls again.
 *
 * On @c Complete, @c consumed tells the caller how many bytes to drop. Anything
 * left in the buffer is the body, or the start of the next pipelined request —
 * which is why @c consumed is returned rather than assumed to be the whole
 * buffer.
 *
 * ### Rejections, and what each one prevents
 *
 * | Condition | Result | Reason |
 * |---|---|---|
 * | Head exceeds 16 KB with no blank line | 431 | Memory exhaustion from a client that never terminates the head |
 * | Request line over 8 KB | 414 | Same, bounded earlier and more specifically |
 * | Target not starting with '/' | 400 | Blocks proxy-style `GET http://host/` |
 * | Bare CR or LF inside the head | 400 | Request smuggling via inconsistent line-ending handling |
 * | Both Content-Length and Transfer-Encoding | 400 | Classic smuggling desync |
 * | Conflicting duplicate Content-Length | 400 | Same |
 * | Any Transfer-Encoding | 501 | Honest refusal beats mis-framing a body we cannot decode |
 * | Missing Host on HTTP/1.1 | 400 | Required by RFC 9112 §3.2 |
 * | Version other than 1.0 or 1.1 | 505 | |
 * | Obsolete line folding | 400 | Ambiguous; RFC 9112 §5.2 allows rejecting |
 */
ParseResult parseRequest(QByteArrayView buffer);

} // namespace http
