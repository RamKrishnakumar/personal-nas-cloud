#include "http/RangeParser.h"

#include <limits>

namespace {

bool isOptionalWhitespace(char c)
{
    return c == ' ' || c == '\t';
}

/** Removes leading and trailing spaces and tabs. */
QByteArrayView trim(QByteArrayView text)
{
    qsizetype first = 0;
    qsizetype last  = text.size();
    while (first < last && isOptionalWhitespace(text[first]))       ++first;
    while (last > first && isOptionalWhitespace(text[last - 1]))     --last;
    return text.sliced(first, last - first);
}

/**
 * @brief Parses a run of ASCII digits.
 * @return The value, or -1 when the input is empty, holds a non-digit, or
 *         would overflow qint64. Rejecting overflow matters: a header of
 *         `bytes=99999999999999999999-` must not wrap into a valid offset.
 */
qint64 parseDecimal(QByteArrayView text)
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

/** Case-insensitive ASCII comparison against a lowercase literal. */
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

} // namespace

namespace http {

ByteRange parseRange(QByteArrayView headerValue, qint64 fileSize)
{
    ByteRange result;

    const QByteArrayView value = trim(headerValue);
    if (value.isEmpty())
        return result;                       // No Range header: serve the whole file.

    // ── Split "bytes" from the range set ──────────────────────────────────
    const qsizetype equals = value.indexOf('=');
    if (equals < 0)
        return result;

    if (!equalsIgnoringCase(trim(value.sliced(0, equals)), "bytes"))
        return result;                       // Unknown unit; RFC says ignore.

    QByteArrayView spec = trim(value.sliced(equals + 1));

    // Only the first range of a multi-range request is honoured.
    const qsizetype comma = spec.indexOf(',');
    if (comma >= 0)
        spec = trim(spec.sliced(0, comma));

    const qsizetype dash = spec.indexOf('-');
    if (dash < 0)
        return result;                       // A range always contains '-'.

    const QByteArrayView firstText = trim(spec.sliced(0, dash));
    const QByteArrayView lastText  = trim(spec.sliced(dash + 1));

    // ── Suffix form: bytes=-n (the final n bytes) ─────────────────────────
    if (firstText.isEmpty()) {
        const qint64 suffixLength = parseDecimal(lastText);
        if (suffixLength < 0)
            return result;                   // Malformed; ignore the header.

        // A zero-length suffix is well formed but can never be served, and an
        // empty file has no bytes to offer. Both are 416, not 200 - the client
        // asked a coherent question with no valid answer.
        result.present = true;
        if (suffixLength == 0 || fileSize == 0)
            return result;

        result.start       = suffixLength >= fileSize ? 0 : fileSize - suffixLength;
        result.end         = fileSize - 1;
        result.satisfiable = true;
        return result;
    }

    // ── Explicit forms: bytes=a-  and  bytes=a-b ──────────────────────────
    const qint64 first = parseDecimal(firstText);
    if (first < 0)
        return result;

    qint64 last;
    if (lastText.isEmpty()) {
        last = fileSize - 1;                 // Open ended.
    } else {
        last = parseDecimal(lastText);
        if (last < 0)
            return result;

        // last < first is invalid syntax, which RFC 9110 requires us to ignore
        // rather than answer with 416.
        if (last < first)
            return result;

        if (last > fileSize - 1)
            last = fileSize - 1;             // Clamp to the end of the file.
    }

    result.present = true;

    // An empty file, or a start at or past the end, cannot be satisfied.
    if (fileSize == 0 || first >= fileSize)
        return result;

    result.start       = first;
    result.end         = last;
    result.satisfiable = true;
    return result;
}

} // namespace http
