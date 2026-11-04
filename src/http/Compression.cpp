#include "http/Compression.h"

#include <QDebug>

#include <array>

namespace {

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

/** True when a `;q=` parameter in @p parameters is zero. */
bool hasZeroQuality(QByteArrayView parameters)
{
    qsizetype start = 0;
    while (start <= parameters.size()) {
        qsizetype semicolon = parameters.indexOf(';', start);
        if (semicolon < 0)
            semicolon = parameters.size();

        const QByteArrayView parameter = trim(parameters.sliced(start, semicolon - start));
        if (parameter.size() >= 2
            && (parameter[0] == 'q' || parameter[0] == 'Q')) {
            const qsizetype equals = parameter.indexOf('=');
            if (equals > 0) {
                const QByteArrayView value = trim(parameter.sliced(equals + 1));
                // "0", "0.0", "0.000" all mean refused. Anything else is accepted;
                // this server does not rank encodings, it only offers one.
                bool nonZeroDigitSeen = false;
                for (const char c : value)
                    if (c >= '1' && c <= '9')
                        nonZeroDigitSeen = true;
                if (!nonZeroDigitSeen)
                    return true;
            }
        }
        start = semicolon + 1;
    }
    return false;
}

/** Table-driven CRC-32, built once. */
const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> generated{};
        for (quint32 index = 0; index < 256; ++index) {
            quint32 value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            generated[index] = value;
        }
        return generated;
    }();
    return table;
}

void appendLittleEndian32(QByteArray& out, quint32 value)
{
    out.append(char(value & 0xFF));
    out.append(char((value >> 8) & 0xFF));
    out.append(char((value >> 16) & 0xFF));
    out.append(char((value >> 24) & 0xFF));
}

} // namespace

namespace http {

bool acceptsGzip(QByteArrayView acceptEncoding)
{
    if (acceptEncoding.isEmpty())
        return false;

    bool wildcardAccepted = false;

    qsizetype start = 0;
    while (start <= acceptEncoding.size()) {
        qsizetype comma = acceptEncoding.indexOf(',', start);
        if (comma < 0)
            comma = acceptEncoding.size();

        const QByteArrayView entry = trim(acceptEncoding.sliced(start, comma - start));
        start = comma + 1;

        if (entry.isEmpty())
            continue;

        const qsizetype semicolon = entry.indexOf(';');
        const QByteArrayView name =
            trim(semicolon < 0 ? entry : entry.sliced(0, semicolon));
        const QByteArrayView parameters =
            semicolon < 0 ? QByteArrayView() : entry.sliced(semicolon + 1);

        if (equalsIgnoringCase(name, "gzip")) {
            // An explicit gzip entry is decisive either way, so return rather than
            // letting a later wildcard override it.
            return !hasZeroQuality(parameters);
        }

        if (name == QByteArrayView("*") && !hasZeroQuality(parameters))
            wildcardAccepted = true;
    }

    return wildcardAccepted;
}

bool isCompressibleContentType(QByteArrayView contentType)
{
    // Only the type before any parameters matters: "text/html; charset=utf-8".
    const qsizetype semicolon = contentType.indexOf(';');
    const QByteArrayView type =
        trim(semicolon < 0 ? contentType : contentType.sliced(0, semicolon));

    if (type.isEmpty())
        return false;

    // All text/* compresses well, including types not listed individually.
    if (type.startsWith("text/"))
        return true;

    static constexpr QByteArrayView compressible[] = {
        "application/json",
        "application/javascript",
        "application/xml",
        "application/xhtml+xml",
        "image/svg+xml",          // Markup, not a raster image.
        "application/manifest+json",
    };

    for (const QByteArrayView candidate : compressible) {
        if (equalsIgnoringCase(type, candidate))
            return true;
    }

    return false;
}

quint32 gzipCrc32(QByteArrayView data)
{
    const auto& table = crcTable();
    quint32 crc = 0xFFFFFFFFu;

    for (const char byte : data)
        crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xFFu] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFFu;
}

QByteArray gzipCompress(const QByteArray& data, int compressionLevel)
{
    // qCompress returns: [4 bytes big-endian uncompressed size][zlib stream],
    // where the zlib stream is [2 byte header][raw deflate][4 byte adler32].
    // gzip needs the raw deflate in the middle, with its own header and trailer.
    const QByteArray zlib = qCompress(data, compressionLevel);

    // 4 size + 2 zlib header + 4 adler32 is the minimum that can carry a payload.
    constexpr qsizetype MinimumEnvelope = 10;
    if (zlib.size() < MinimumEnvelope)
        return {};

    const unsigned char compressionMethodAndFlags = static_cast<unsigned char>(zlib[4]);
    const unsigned char flags                     = static_cast<unsigned char>(zlib[5]);

    // Validated rather than assumed: if a future Qt emitted a preset dictionary or
    // a different method, blindly slicing would produce a stream every client
    // rejects, and the cause would not be obvious.
    const bool methodIsDeflate   = (compressionMethodAndFlags & 0x0F) == 8;
    const bool hasPresetDictionary = (flags & 0x20) != 0;

    if (!methodIsDeflate || hasPresetDictionary) {
        qWarning() << "[gzip] Unexpected zlib stream from qCompress; "
                      "sending uncompressed.";
        return {};
    }

    const qsizetype deflateSize = zlib.size() - MinimumEnvelope;
    if (deflateSize <= 0)
        return {};

    QByteArray out;
    out.reserve(18 + deflateSize);

    // RFC 1952 header: magic, deflate, no flags, no timestamp, unknown OS.
    // The timestamp is deliberately zero — it is not useful here, and a varying
    // value would make otherwise identical responses differ byte for byte.
    out.append('\x1f');
    out.append('\x8b');
    out.append('\x08');
    out.append('\x00');
    appendLittleEndian32(out, 0);
    out.append('\x00');
    out.append('\xff');

    out.append(zlib.constData() + 6, deflateSize);

    appendLittleEndian32(out, gzipCrc32(data));
    appendLittleEndian32(out, static_cast<quint32>(data.size() & 0xFFFFFFFFu));

    return out;
}

} // namespace http
