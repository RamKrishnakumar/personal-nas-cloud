#pragma once

#include <QByteArray>
#include <QByteArrayView>

namespace http {

struct Request;

/**
 * @brief Value of cookie @p name from a `Cookie:` header value.
 *
 * Returns an empty array when absent. Pure, so the awkward cases are testable
 * without a socket.
 *
 * ### What it has to get right
 *
 * Cookie values are not percent-decoded and may contain `=`, so only the first
 * `=` in each pair separates name from value. Matching has to be on the whole
 * name: a prefix match would let a cookie called `nd_session_decoy` answer a
 * lookup for `nd_session`. Names are case-sensitive, per RFC 6265.
 */
QByteArray cookieValue(QByteArrayView cookieHeader, QByteArrayView name);

/** Convenience overload reading the request's own `Cookie:` header. */
QByteArray cookieValue(const Request& request, QByteArrayView name);

} // namespace http
