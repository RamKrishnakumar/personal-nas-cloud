#include "http/Cookies.h"

#include "http/RequestParser.h"

namespace {

/** Drops leading and trailing spaces and tabs. */
QByteArrayView trimmed(QByteArrayView value)
{
    qsizetype begin = 0;
    qsizetype end   = value.size();

    while (begin < end && (value[begin] == ' ' || value[begin] == '\t'))
        ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
        --end;

    return value.sliced(begin, end - begin);
}

} // namespace

namespace http {

QByteArray cookieValue(QByteArrayView cookieHeader, QByteArrayView name)
{
    if (name.isEmpty())
        return {};

    qsizetype position = 0;

    while (position < cookieHeader.size()) {
        qsizetype separator = cookieHeader.indexOf(';', position);
        if (separator < 0)
            separator = cookieHeader.size();

        const QByteArrayView pair =
            trimmed(cookieHeader.sliced(position, separator - position));

        // Only the first '=' splits: a value may legitimately contain more, and
        // base64url padding is exactly that case.
        const qsizetype equals = pair.indexOf('=');
        if (equals > 0 && trimmed(pair.first(equals)) == name)
            return trimmed(pair.sliced(equals + 1)).toByteArray();

        position = separator + 1;
    }

    return {};
}

QByteArray cookieValue(const Request& request, QByteArrayView name)
{
    return cookieValue(request.header("cookie"), name);
}

} // namespace http
