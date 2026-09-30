#include <QTest>

#include "http/RangeParser.h"

using http::ByteRange;
using http::parseRange;

/**
 * Tests for http::parseRange.
 *
 * The three outcomes are distinct and must not be conflated:
 *   present == false            -> 200, whole file
 *   present && !satisfiable     -> 416
 *   present && satisfiable      -> 206 with an exact length
 *
 * The v0.5 implementation returned only a start offset and treated 0 as "no
 * range", so `bytes=0-1` fell through to a full 200 response. The regression
 * tests for that case are marked below.
 */
class TestRangeParser : public QObject
{
    Q_OBJECT

private slots:
    void noHeader();
    void fullRange_data();
    void fullRange();
    void openEnded_data();
    void openEnded();
    void suffix_data();
    void suffix();
    void ignored_data();
    void ignored();
    void unsatisfiable_data();
    void unsatisfiable();
    void clampsPastEnd();
    void multiRangeTakesFirst();
    void toleratesWhitespaceAndCase();
    void rejectsOverflow();
    void emptyFile();
};

// ─────────────────────────────────────────────────────────────────────────────

void TestRangeParser::noHeader()
{
    const ByteRange r = parseRange(QByteArrayView(), 1000);
    QVERIFY(!r.present);
    QCOMPARE(r.length(), 0);
}

void TestRangeParser::fullRange_data()
{
    QTest::addColumn<QByteArray>("header");
    QTest::addColumn<qint64>("size");
    QTest::addColumn<qint64>("start");
    QTest::addColumn<qint64>("end");

    // The probe a <video> element sends first. v0.5 answered this with the
    // entire file, which is the video-latency bug.
    QTest::newRow("video probe 0-1")   << QByteArray("bytes=0-1")      << 1000LL << 0LL   << 1LL;
    QTest::newRow("first kilobyte")    << QByteArray("bytes=0-1023")   << 4096LL << 0LL   << 1023LL;
    QTest::newRow("middle")            << QByteArray("bytes=100-200")  << 1000LL << 100LL << 200LL;
    QTest::newRow("single byte")       << QByteArray("bytes=5-5")      << 1000LL << 5LL   << 5LL;
    QTest::newRow("exactly the file")  << QByteArray("bytes=0-999")    << 1000LL << 0LL   << 999LL;
    QTest::newRow("last byte")         << QByteArray("bytes=999-999")  << 1000LL << 999LL << 999LL;
}

void TestRangeParser::fullRange()
{
    QFETCH(QByteArray, header);
    QFETCH(qint64, size);
    QFETCH(qint64, start);
    QFETCH(qint64, end);

    const ByteRange r = parseRange(header, size);
    QVERIFY(r.present);
    QVERIFY(r.satisfiable);
    QCOMPARE(r.start, start);
    QCOMPARE(r.end, end);
    QCOMPARE(r.length(), end - start + 1);
}

void TestRangeParser::openEnded_data()
{
    QTest::addColumn<QByteArray>("header");
    QTest::addColumn<qint64>("size");
    QTest::addColumn<qint64>("start");

    QTest::newRow("from zero")  << QByteArray("bytes=0-")   << 1000LL << 0LL;
    QTest::newRow("mid file")   << QByteArray("bytes=500-") << 1000LL << 500LL;
    QTest::newRow("last byte")  << QByteArray("bytes=999-") << 1000LL << 999LL;
}

void TestRangeParser::openEnded()
{
    QFETCH(QByteArray, header);
    QFETCH(qint64, size);
    QFETCH(qint64, start);

    const ByteRange r = parseRange(header, size);
    QVERIFY(r.present);
    QVERIFY(r.satisfiable);
    QCOMPARE(r.start, start);
    QCOMPARE(r.end, size - 1);
    QCOMPARE(r.length(), size - start);
}

void TestRangeParser::suffix_data()
{
    QTest::addColumn<QByteArray>("header");
    QTest::addColumn<qint64>("size");
    QTest::addColumn<qint64>("start");

    QTest::newRow("last 500")            << QByteArray("bytes=-500")  << 1000LL << 500LL;
    QTest::newRow("last byte")           << QByteArray("bytes=-1")    << 1000LL << 999LL;
    QTest::newRow("whole file")          << QByteArray("bytes=-1000") << 1000LL << 0LL;
    // A suffix longer than the file clamps to the start rather than failing.
    QTest::newRow("longer than file")    << QByteArray("bytes=-5000") << 1000LL << 0LL;
}

void TestRangeParser::suffix()
{
    QFETCH(QByteArray, header);
    QFETCH(qint64, size);
    QFETCH(qint64, start);

    const ByteRange r = parseRange(header, size);
    QVERIFY(r.present);
    QVERIFY(r.satisfiable);
    QCOMPARE(r.start, start);
    QCOMPARE(r.end, size - 1);
}

void TestRangeParser::ignored_data()
{
    QTest::addColumn<QByteArray>("header");

    // RFC 9110 14.2: an unparsable Range header must be ignored, producing a
    // normal 200 - never a 416.
    QTest::newRow("empty")             << QByteArray("");
    QTest::newRow("no equals")         << QByteArray("bytes 0-100");
    QTest::newRow("no dash")           << QByteArray("bytes=100");
    QTest::newRow("unknown unit")      << QByteArray("items=0-100");
    QTest::newRow("bare dash")         << QByteArray("bytes=-");
    QTest::newRow("non numeric start") << QByteArray("bytes=abc-100");
    QTest::newRow("non numeric end")   << QByteArray("bytes=0-abc");
    QTest::newRow("negative looking")  << QByteArray("bytes=--100");
    QTest::newRow("end before start")  << QByteArray("bytes=500-100");
    QTest::newRow("garbage")           << QByteArray("!!!");
}

void TestRangeParser::ignored()
{
    QFETCH(QByteArray, header);

    const ByteRange r = parseRange(header, 1000);
    QVERIFY2(!r.present,
             "A malformed Range must be ignored (200), not rejected (416)");
}

void TestRangeParser::unsatisfiable_data()
{
    QTest::addColumn<QByteArray>("header");
    QTest::addColumn<qint64>("size");

    // Well formed, but no valid answer exists. These are the only 416 cases.
    QTest::newRow("start at size")    << QByteArray("bytes=1000-")     << 1000LL;
    QTest::newRow("start past size")  << QByteArray("bytes=5000-")     << 1000LL;
    QTest::newRow("range past size")  << QByteArray("bytes=2000-3000") << 1000LL;
    QTest::newRow("zero suffix")      << QByteArray("bytes=-0")        << 1000LL;
}

void TestRangeParser::unsatisfiable()
{
    QFETCH(QByteArray, header);
    QFETCH(qint64, size);

    const ByteRange r = parseRange(header, size);
    QVERIFY2(r.present, "A well-formed range must be reported as present");
    QVERIFY2(!r.satisfiable, "Out-of-bounds range must be unsatisfiable (416)");
    QCOMPARE(r.length(), 0);
}

void TestRangeParser::clampsPastEnd()
{
    // A client may legitimately ask past the end; the server clamps instead of
    // failing, and must never report a length longer than the file.
    const ByteRange r = parseRange("bytes=900-5000", 1000);
    QVERIFY(r.satisfiable);
    QCOMPARE(r.start, 900);
    QCOMPARE(r.end, 999);
    QCOMPARE(r.length(), 100);
}

void TestRangeParser::multiRangeTakesFirst()
{
    const ByteRange r = parseRange("bytes=0-99,200-299", 1000);
    QVERIFY(r.satisfiable);
    QCOMPARE(r.start, 0);
    QCOMPARE(r.end, 99);
}

void TestRangeParser::toleratesWhitespaceAndCase()
{
    for (const QByteArray header : { QByteArray("BYTES=0-99"),
                                     QByteArray("Bytes = 0-99"),
                                     QByteArray("  bytes=0-99  "),
                                     QByteArray("bytes= 0 - 99 ") }) {
        const ByteRange r = parseRange(header, 1000);
        QVERIFY2(r.satisfiable, header.constData());
        QCOMPARE(r.start, 0);
        QCOMPARE(r.end, 99);
    }
}

void TestRangeParser::rejectsOverflow()
{
    // Must not wrap into a valid offset.
    const ByteRange r = parseRange("bytes=99999999999999999999-", 1000);
    QVERIFY(!r.present);
}

void TestRangeParser::emptyFile()
{
    // A zero-byte file can satisfy no range at all.
    for (const QByteArray header : { QByteArray("bytes=0-"),
                                     QByteArray("bytes=0-0"),
                                     QByteArray("bytes=-1") }) {
        const ByteRange r = parseRange(header, 0);
        QVERIFY2(r.present, header.constData());
        QVERIFY2(!r.satisfiable, header.constData());
    }
}

QTEST_APPLESS_MAIN(TestRangeParser)
#include "tst_rangeparser.moc"
