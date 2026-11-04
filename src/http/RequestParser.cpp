#include "http/RequestParser.h"

#include <limits>

namespace {

constexpr QByteArrayView CRLF  = "\r\n";
constexpr QByteArrayView CRLF2 = "\r\n\r\n";

bool isOptionalWhitespace(char c)
{
    return c == ' ' || c == '\t';
}

QByteArrayView trim(QByteArrayView text)
{
    qsizetype first = 0;
    qsizetype last  = text.size();
    while (first < last && isOptionalWhitespace(text[first]))    ++first;
    while (last > first && isOptionalWhitespace(text[last - 1])) --last;
    return text.sliced(first, last - first);
}

QByteArray toLowerAscii(QByteArrayView text)
{
    QByteArray out(text.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        out[i] = c;
    }
    return out;
}

bool equalsIgnoringCase(QByteArrayView text, QByteArrayView lowercaseLiteral)
{
    if (text.size() != lowercaseLiteral.size())
        return false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != lowercaseLiteral[i])
            return false;
    }
    return true;
}

/**
 * True when a comma-separated list header contains @p token.
 * `Connection: keep-alive, Upgrade` must match "keep-alive".
 */
bool listContainsToken(QByteArrayView value, QByteArrayView lowercaseToken)
{
    qsizetype start = 0;
    while (start <= value.size()) {
        qsizetype comma = value.indexOf(',', start);
        if (comma < 0)
            comma = value.size();
        if (equalsIgnoringCase(trim(value.sliced(start, comma - start)), lowercaseToken))
            return true;
        start = comma + 1;
    }
    return false;
}

qint64 parseContentLength(QByteArrayView text)
{
    if (text.isEmpty())
        return -1;
    qint64 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return -1;
        const int digit = c - '0';
        if (value > (std::numeric_limits<qint64>::max() - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    return value;
}

/** Rejects bytes that must never appear unescaped in a request line. */
bool hasForbiddenControl(QByteArrayView text)
{
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F)
            return true;
    }
    return false;
}

/** Header values may contain horizontal tab, but no other control byte. */
bool hasForbiddenControlAllowingTab(QByteArrayView text)
{
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\t')
            continue;
        if (u < 0x20 || u == 0x7F)
            return true;
    }
    return false;
}

http::ParseResult fail(http::ParseStatus status)
{
    http::ParseResult result;
    result.status = status;
    return result;
}

} // namespace

namespace http {

QByteArrayView Request::header(QByteArrayView lowercaseName) const
{
    for (const auto& [name, value] : headers) {
        if (QByteArrayView(name) == lowercaseName)
            return value;
    }
    return {};
}

int Request::headerCount(QByteArrayView lowercaseName) const
{
    int count = 0;
    for (const auto& [name, value] : headers) {
        Q_UNUSED(value)
        if (QByteArrayView(name) == lowercaseName)
            ++count;
    }
    return count;
}

ParseResult parseRequest(QByteArrayView buffer)
{
    // ── Locate the end of the head ────────────────────────────────────────
    const qsizetype blankLine = buffer.indexOf(CRLF2);
    if (blankLine < 0) {
        // Bound memory before the client has committed to terminating the head.
        // Without this, a peer that opens a socket and dribbles header bytes
        // forever grows the buffer without limit.
        if (buffer.size() > MaxHeaderBlockBytes)
            return fail(ParseStatus::HeadersTooLarge);
        return {};                                  // Incomplete: read more.
    }

    const qsizetype headEnd  = blankLine + CRLF2.size();
    if (headEnd > MaxHeaderBlockBytes)
        return fail(ParseStatus::HeadersTooLarge);

    const QByteArrayView head = buffer.sliced(0, blankLine + CRLF.size());

    // ── Request line ──────────────────────────────────────────────────────
    const qsizetype lineEnd = head.indexOf(CRLF);
    if (lineEnd < 0)
        return fail(ParseStatus::BadRequest);

    const QByteArrayView requestLine = head.sliced(0, lineEnd);
    if (requestLine.size() > MaxRequestLineBytes)
        return fail(ParseStatus::UriTooLong);

    // A bare CR or LF here means the client's idea of where the line ends
    // differs from ours, which is the foundation of request smuggling.
    if (hasForbiddenControl(requestLine))
        return fail(ParseStatus::BadRequest);

    const qsizetype firstSpace = requestLine.indexOf(' ');
    if (firstSpace <= 0)
        return fail(ParseStatus::BadRequest);

    const qsizetype secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (secondSpace < 0)
        return fail(ParseStatus::BadRequest);

    // Exactly three tokens. A space inside the target must be percent-encoded;
    // accepting a third space would let "GET /a HTTP/1.1 junk" through.
    if (requestLine.indexOf(' ', secondSpace + 1) >= 0)
        return fail(ParseStatus::BadRequest);

    const QByteArrayView method  = requestLine.sliced(0, firstSpace);
    const QByteArrayView target  = requestLine.sliced(firstSpace + 1,
                                                      secondSpace - firstSpace - 1);
    const QByteArrayView version = requestLine.sliced(secondSpace + 1);

    if (method.isEmpty() || target.isEmpty())
        return fail(ParseStatus::BadRequest);

    // Origin-form only. Refusing absolute-form blocks "GET http://elsewhere/"
    // being used to make this server behave like an open proxy.
    if (target[0] != '/')
        return fail(ParseStatus::BadRequest);

    if (!version.startsWith("HTTP/"))
        return fail(ParseStatus::BadRequest);

    const QByteArrayView versionNumber = version.sliced(5);
    int minorVersion = 1;
    if (versionNumber == "1.1")
        minorVersion = 1;
    else if (versionNumber == "1.0")
        minorVersion = 0;
    else
        return fail(ParseStatus::VersionNotSupported);

    // ── Headers ───────────────────────────────────────────────────────────
    ParseResult result;
    Request&    request = result.request;

    request.method       = method.toByteArray();
    request.target       = target.toByteArray();
    request.minorVersion = minorVersion;

    const qsizetype questionMark = target.indexOf('?');
    if (questionMark < 0) {
        request.path = target.toByteArray();
    } else {
        request.path  = target.sliced(0, questionMark).toByteArray();
        request.query = target.sliced(questionMark + 1).toByteArray();
    }

    qsizetype cursor = lineEnd + CRLF.size();
    while (cursor < head.size()) {
        const qsizetype nextCrlf = head.indexOf(CRLF, cursor);
        if (nextCrlf < 0)
            return fail(ParseStatus::BadRequest);

        const QByteArrayView line = head.sliced(cursor, nextCrlf - cursor);
        cursor = nextCrlf + CRLF.size();

        if (line.isEmpty())
            break;                                  // Reached the blank line.

        // Obsolete line folding: a continuation line begins with whitespace.
        // RFC 9112 5.2 permits rejecting it, and unfolding is a known source of
        // header-smuggling bugs.
        if (isOptionalWhitespace(line[0]))
            return fail(ParseStatus::BadRequest);

        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            return fail(ParseStatus::BadRequest);

        const QByteArrayView rawName  = line.sliced(0, colon);
        const QByteArrayView rawValue = trim(line.sliced(colon + 1));

        // No space permitted between the name and the colon.
        if (isOptionalWhitespace(rawName[rawName.size() - 1]))
            return fail(ParseStatus::BadRequest);

        if (hasForbiddenControl(rawName) || hasForbiddenControlAllowingTab(rawValue))
            return fail(ParseStatus::BadRequest);

        request.headers.emplace_back(toLowerAscii(rawName), rawValue.toByteArray());
    }

    // ── Framing checks ────────────────────────────────────────────────────
    const bool hasTransferEncoding = request.headerCount("transfer-encoding") > 0;
    const bool hasContentLength    = request.headerCount("content-length") > 0;

    // Sending both lets two hops disagree about where the body ends, which is
    // the classic smuggling desync. Refuse rather than pick a winner.
    if (hasTransferEncoding && hasContentLength)
        return fail(ParseStatus::BadRequest);

    if (hasTransferEncoding)
        return fail(ParseStatus::NotImplemented);

    if (hasContentLength) {
        qint64 agreedLength = -1;
        for (const auto& [name, value] : request.headers) {
            if (QByteArrayView(name) != QByteArrayView("content-length"))
                continue;

            const qint64 parsed = parseContentLength(trim(value));
            if (parsed < 0)
                return fail(ParseStatus::BadRequest);

            // Repeated identical values are legal; differing ones are not.
            if (agreedLength >= 0 && agreedLength != parsed)
                return fail(ParseStatus::BadRequest);

            agreedLength = parsed;
        }
        request.contentLength = agreedLength;
    }

    if (minorVersion == 1 && request.headerCount("host") != 1)
        return fail(ParseStatus::BadRequest);

    // ── Connection reuse ──────────────────────────────────────────────────
    const QByteArrayView connection = request.header("connection");
    if (minorVersion == 1)
        request.keepAlive = !listContainsToken(connection, "close");
    else
        request.keepAlive = listContainsToken(connection, "keep-alive");

    result.status   = ParseStatus::Complete;
    result.consumed = headEnd;
    return result;
}

} // namespace http
