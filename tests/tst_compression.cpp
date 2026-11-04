#include <QTest>

#include "http/Compression.h"

using http::acceptsGzip;
using http::gzipCompress;
using http::gzipCrc32;
using http::isCompressibleContentType;

namespace {

quint32 readLittleEndian32(const QByteArray& data, qsizetype offset)
{
    return quint32(quint8(data[offset]))
         | (quint32(quint8(data[offset + 1])) << 8)
         | (quint32(quint8(data[offset + 2])) << 16)
         | (quint32(quint8(data[offset + 3])) << 24);
}

/** Markup-like input, so compression behaves as it will in production. */
QByteArray listingLikeBody(int entries)
{
    QByteArray body = "<!DOCTYPE html><html><body><div class='grid'>";
    for (int i = 0; i < entries; ++i) {
        body += "<a class='card' href='/Photos/IMG_" + QByteArray::number(i)
              + ".jpg?download=1'><div class='thumb'>"
                "<img src='/_thumb/s/Photos/IMG_" + QByteArray::number(i)
              + ".jpg' loading='lazy'></div><div class='info'><b>IMG_"
              + QByteArray::number(i) + ".jpg</b><small>3 MB</small></div></a>";
    }
    return body + "</div></body></html>";
}

} // namespace

/**
 * Tests for http::Compression.
 *
 * The gzip framing is assembled by hand around qCompress, so the trailer is where
 * a mistake would hide: a wrong CRC or length produces a stream every client
 * rejects, which from the outside looks like a broken server rather than a
 * compression bug.
 */
class TestCompression : public QObject
{
    Q_OBJECT

private slots:
    // ── Negotiation ──────────────────────────────────────────────────────
    void acceptsGzipCases_data();
    void acceptsGzipCases();

    void compressibleTypesCases_data();
    void compressibleTypesCases();

    // ── CRC ──────────────────────────────────────────────────────────────
    void crc32MatchesPublishedVectors();

    // ── Framing ──────────────────────────────────────────────────────────
    void producesAValidGzipHeader();
    void trailerCarriesCrcAndLength();
    void compressesMarkupSubstantially();
    void handlesEmptyAndTinyInput();
    void handlesBinaryInputWithNulBytes();
    void isDeterministic();
    void handlesLargeInput();
};

// ─────────────────────────────────────────────────────────────────────────────
//  Negotiation
// ─────────────────────────────────────────────────────────────────────────────

void TestCompression::acceptsGzipCases_data()
{
    QTest::addColumn<QByteArray>("header");
    QTest::addColumn<bool>("expected");

    QTest::newRow("absent")             << QByteArray("")                      << false;
    QTest::newRow("gzip only")          << QByteArray("gzip")                  << true;
    QTest::newRow("browser typical")    << QByteArray("gzip, deflate, br")     << true;
    QTest::newRow("mixed case")         << QByteArray("GZip")                  << true;
    QTest::newRow("with quality")       << QByteArray("gzip;q=1.0")            << true;
    QTest::newRow("low but non-zero")   << QByteArray("gzip;q=0.1")            << true;
    QTest::newRow("spaces")             << QByteArray("  gzip ; q=0.8 ")       << true;
    QTest::newRow("wildcard")           << QByteArray("*")                     << true;
    QTest::newRow("deflate only")       << QByteArray("deflate")               << false;
    QTest::newRow("br only")            << QByteArray("br")                    << false;

    // An explicit refusal must win, even alongside a wildcard.
    QTest::newRow("gzip refused")       << QByteArray("gzip;q=0")              << false;
    QTest::newRow("gzip refused zeros") << QByteArray("gzip;q=0.000")          << false;
    QTest::newRow("refused with star")  << QByteArray("gzip;q=0, *")           << false;
    QTest::newRow("wildcard refused")   << QByteArray("*;q=0")                 << false;
}

void TestCompression::acceptsGzipCases()
{
    QFETCH(QByteArray, header);
    QFETCH(bool, expected);
    QCOMPARE(acceptsGzip(header), expected);
}

void TestCompression::compressibleTypesCases_data()
{
    QTest::addColumn<QByteArray>("contentType");
    QTest::addColumn<bool>("expected");

    QTest::newRow("html")        << QByteArray("text/html; charset=utf-8") << true;
    QTest::newRow("plain")       << QByteArray("text/plain")               << true;
    QTest::newRow("css")         << QByteArray("text/css; charset=utf-8")  << true;
    QTest::newRow("js")          << QByteArray("text/javascript")          << true;
    QTest::newRow("json")        << QByteArray("application/json")         << true;
    QTest::newRow("svg")         << QByteArray("image/svg+xml")            << true;
    QTest::newRow("xml")         << QByteArray("application/xml")          << true;

    // Already compressed: deflating these costs CPU and can grow the body.
    QTest::newRow("jpeg")        << QByteArray("image/jpeg")               << false;
    QTest::newRow("webp")        << QByteArray("image/webp")               << false;
    QTest::newRow("png")         << QByteArray("image/png")                << false;
    QTest::newRow("mp4")         << QByteArray("video/mp4")                << false;
    QTest::newRow("zip")         << QByteArray("application/zip")          << false;
    QTest::newRow("octets")      << QByteArray("application/octet-stream") << false;
    QTest::newRow("empty")       << QByteArray("")                         << false;
}

void TestCompression::compressibleTypesCases()
{
    QFETCH(QByteArray, contentType);
    QFETCH(bool, expected);
    QCOMPARE(isCompressibleContentType(contentType), expected);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CRC
// ─────────────────────────────────────────────────────────────────────────────

void TestCompression::crc32MatchesPublishedVectors()
{
    // Published values for the gzip polynomial. A hand-rolled CRC that is subtly
    // wrong still produces plausible-looking bytes, so this is checked against
    // known answers rather than against itself.
    QCOMPARE(gzipCrc32(QByteArrayView("")), 0x00000000u);
    QCOMPARE(gzipCrc32(QByteArrayView("a")), 0xE8B7BE43u);
    QCOMPARE(gzipCrc32(QByteArrayView("abc")), 0x352441C2u);
    QCOMPARE(gzipCrc32(QByteArrayView("123456789")), 0xCBF43926u);
    QCOMPARE(gzipCrc32(QByteArrayView("The quick brown fox jumps over the lazy dog")),
             0x414FA339u);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Framing
// ─────────────────────────────────────────────────────────────────────────────

void TestCompression::producesAValidGzipHeader()
{
    const QByteArray body       = listingLikeBody(200);
    const QByteArray compressed = gzipCompress(body);

    QVERIFY(!compressed.isEmpty());
    QVERIFY(compressed.size() > 18);

    QCOMPARE(quint8(compressed[0]), 0x1Fu);   // Magic
    QCOMPARE(quint8(compressed[1]), 0x8Bu);
    QCOMPARE(quint8(compressed[2]), 0x08u);   // Deflate
    QCOMPARE(quint8(compressed[3]), 0x00u);   // No optional fields

    // Zero timestamp, so identical input yields identical output.
    QCOMPARE(readLittleEndian32(compressed, 4), 0u);
}

void TestCompression::trailerCarriesCrcAndLength()
{
    const QByteArray body       = listingLikeBody(150);
    const QByteArray compressed = gzipCompress(body);
    QVERIFY(!compressed.isEmpty());

    // The trailer is what a client checks after inflating. If either field is
    // wrong the whole response is discarded, having already been transferred.
    const quint32 crc  = readLittleEndian32(compressed, compressed.size() - 8);
    const quint32 size = readLittleEndian32(compressed, compressed.size() - 4);

    QCOMPARE(crc, gzipCrc32(body));
    QCOMPARE(size, quint32(body.size()));
}

void TestCompression::compressesMarkupSubstantially()
{
    // The reason step 9 exists. A listing is highly repetitive markup; if the
    // saving were marginal there would be no point paying the CPU cost.
    const QByteArray body       = listingLikeBody(500);
    const QByteArray compressed = gzipCompress(body);

    QVERIFY(!compressed.isEmpty());
    QVERIFY2(compressed.size() * 4 < body.size(),
             qPrintable(QStringLiteral("compressed %1 of %2 bytes (%3%)")
                            .arg(compressed.size()).arg(body.size())
                            .arg(100.0 * compressed.size() / body.size(), 0, 'f', 1)));
}

void TestCompression::handlesEmptyAndTinyInput()
{
    // Empty input has no zlib stream to wrap — qCompress returns only its length
    // prefix — so the documented "failed, send it uncompressed" result is correct
    // rather than a malformed one-byte gzip stream.
    QVERIFY2(gzipCompress(QByteArray()).isEmpty(),
             "Empty input must report failure so the caller falls back");

    // Tiny non-empty bodies are never compressed in practice, being well under
    // MinimumCompressibleBytes, but must still produce a valid stream if asked.
    for (const QByteArray input : { QByteArray("x"), QByteArray("hello world") }) {
        const QByteArray compressed = gzipCompress(input);
        QVERIFY2(!compressed.isEmpty(), input.constData());
        QCOMPARE(quint8(compressed[0]), 0x1Fu);
        QCOMPARE(readLittleEndian32(compressed, compressed.size() - 8), gzipCrc32(input));
        QCOMPARE(readLittleEndian32(compressed, compressed.size() - 4),
                 quint32(input.size()));
    }
}

void TestCompression::handlesBinaryInputWithNulBytes()
{
    // Nothing here may treat the body as a C string.
    QByteArray input;
    for (int i = 0; i < 4096; ++i)
        input.append(char(i % 256));

    const QByteArray compressed = gzipCompress(input);
    QVERIFY(!compressed.isEmpty());
    QCOMPARE(readLittleEndian32(compressed, compressed.size() - 8), gzipCrc32(input));
    QCOMPARE(readLittleEndian32(compressed, compressed.size() - 4),
             quint32(input.size()));
}

void TestCompression::isDeterministic()
{
    // Identical responses must be byte-identical, so an ETag over the compressed
    // body would stay stable.
    const QByteArray body = listingLikeBody(80);
    QCOMPARE(gzipCompress(body), gzipCompress(body));
}

void TestCompression::handlesLargeInput()
{
    // A listing of a few thousand files is the realistic upper end.
    const QByteArray body = listingLikeBody(5000);
    QVERIFY(body.size() > 500'000);

    const QByteArray compressed = gzipCompress(body);
    QVERIFY(!compressed.isEmpty());
    QCOMPARE(readLittleEndian32(compressed, compressed.size() - 4),
             quint32(body.size()));
    QVERIFY(compressed.size() < body.size() / 4);
}

QTEST_APPLESS_MAIN(TestCompression)
#include "tst_compression.moc"
