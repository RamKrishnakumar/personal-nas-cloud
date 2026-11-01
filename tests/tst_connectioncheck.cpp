#include "core/Net/ConnectionCheck.h"

#include <QtTest>

/**
 * @file
 * @brief Coverage for core::classifyConnection().
 *
 * The decision that steers the whole of remote access: whether forwarding a port on
 * the router can possibly work. Getting it wrong in either direction wastes somebody's
 * evening — a false "public" sends them to configure a router rule that can never
 * match, and a false "behind NAT" tells them to install an app they did not need.
 *
 * Pure, so every branch is reachable here without a network, a router, or either of
 * the two internet connections involved.
 */
class TestConnectionCheck : public QObject
{
    Q_OBJECT

private slots:
    void matchingAddressesMeanAPublicIp();
    void differingAddressesMeanCarrierNat();
    void ignoresSurroundingWhitespace();

    void reservedRangeMeansCarrierNatWithoutTheRouter();
    void squatSpaceIsUnknownWithoutTheRouter();
    void ordinaryAddressIsUnknownWithoutTheRouter();

    void noPublicAddressIsUnknown();
    void noPublicAddressIsUnknownEvenWithARouterAnswer();

    void roundTripsThroughConfiguration_data();
    void roundTripsThroughConfiguration();
    void unrecognisedConfigurationReadsAsUnknown();
    void everyTypeHasADescription();
};

void TestConnectionCheck::matchingAddressesMeanAPublicIp()
{
    // The BSNL connection: the router holds the address the internet sees, so a
    // forwarding rule on it has something to match.
    QCOMPARE(core::classifyConnection(QStringLiteral("111.91.226.113"),
                                      QStringLiteral("111.91.226.113")),
             core::ConnectionType::PublicIp);
}

void TestConnectionCheck::differingAddressesMeanCarrierNat()
{
    // The measurement from the Airtel connection, exactly as reported. This is the
    // case the whole feature exists for, and the one a range check gets wrong.
    QCOMPARE(core::classifyConnection(QStringLiteral("172.40.42.138"),
                                      QStringLiteral("111.91.226.113")),
             core::ConnectionType::CarrierNat);
}

void TestConnectionCheck::ignoresSurroundingWhitespace()
{
    // One of these comes from an HTTP body and the other from XML. Both arrive with
    // whitespace often enough that comparing them raw would report carrier-grade NAT
    // on a connection that has none — a false negative caused entirely by a newline.
    QCOMPARE(core::classifyConnection(QStringLiteral(" 203.0.113.9 "),
                                      QStringLiteral("203.0.113.9\n")),
             core::ConnectionType::PublicIp);
}

void TestConnectionCheck::reservedRangeMeansCarrierNatWithoutTheRouter()
{
    // UPnP gave nothing, but 100.64.0.0/10 is reserved by RFC 6598 and is never
    // routable on the public internet, so it settles the question on its own.
    QCOMPARE(core::classifyConnection(QString(),
                                      QStringLiteral("100.90.1.2")),
             core::ConnectionType::CarrierNat);
}

void TestConnectionCheck::squatSpaceIsUnknownWithoutTheRouter()
{
    // The important negative result, and the reason this is not just a range check.
    //
    // 111.91.226.113 is what the internet reports on the Airtel connection, and it
    // is an ordinary routable-looking address. The connection IS behind the
    // provider's NAT, but nothing about this address alone reveals that — the
    // evidence was the router's disagreeing 172.40.42.138. With no answer from the
    // router, the honest verdict is that we do not know.
    QCOMPARE(core::classifyConnection(QString(),
                                      QStringLiteral("111.91.226.113")),
             core::ConnectionType::Unknown);
}

void TestConnectionCheck::ordinaryAddressIsUnknownWithoutTheRouter()
{
    QCOMPARE(core::classifyConnection(QString(), QStringLiteral("203.0.113.9")),
             core::ConnectionType::Unknown);

    // 100.64.0.0/10 and not merely "starts with 100": 100.200.x is outside the
    // reserved block and ordinary public space.
    QCOMPARE(core::classifyConnection(QString(), QStringLiteral("100.200.1.2")),
             core::ConnectionType::Unknown);
}

void TestConnectionCheck::noPublicAddressIsUnknown()
{
    // Offline, or the lookup failed. Nothing to compare against.
    QCOMPARE(core::classifyConnection(QString(), QString()),
             core::ConnectionType::Unknown);
}

void TestConnectionCheck::noPublicAddressIsUnknownEvenWithARouterAnswer()
{
    // The router's own idea of its address proves nothing by itself: on the Airtel
    // connection it reports a number that looks public and is not reachable.
    QCOMPARE(core::classifyConnection(QStringLiteral("172.40.42.138"), QString()),
             core::ConnectionType::Unknown);
}

void TestConnectionCheck::roundTripsThroughConfiguration_data()
{
    QTest::addColumn<int>("type");

    QTest::newRow("unknown")    << int(core::ConnectionType::Unknown);
    QTest::newRow("public")     << int(core::ConnectionType::PublicIp);
    QTest::newRow("carrierNat") << int(core::ConnectionType::CarrierNat);
}

void TestConnectionCheck::roundTripsThroughConfiguration()
{
    QFETCH(int, type);
    const auto original = core::ConnectionType(type);

    // The verdict is cached in config.ini so an Unknown result can be settled once by
    // hand rather than asked at every launch. A type that did not survive the trip
    // would turn that convenience into a setting that silently reverted.
    QCOMPARE(core::connectionTypeFromString(core::connectionTypeToString(original)),
             original);
}

void TestConnectionCheck::unrecognisedConfigurationReadsAsUnknown()
{
    // A hand-edited config file, or one written by a future version. Unknown is the
    // safe reading: it prompts a check rather than asserting something wrong.
    QCOMPARE(core::connectionTypeFromString(QStringLiteral("something else")),
             core::ConnectionType::Unknown);
    QCOMPARE(core::connectionTypeFromString(QString()),
             core::ConnectionType::Unknown);

    // Case and padding are tolerated, because people edit this file.
    QCOMPARE(core::connectionTypeFromString(QStringLiteral("  CarrierNAT ")),
             core::ConnectionType::CarrierNat);
}

void TestConnectionCheck::everyTypeHasADescription()
{
    for (const auto type : { core::ConnectionType::Unknown,
                             core::ConnectionType::PublicIp,
                             core::ConnectionType::CarrierNat }) {
        QVERIFY(!core::describeConnectionType(type).isEmpty());
    }
}

QTEST_MAIN(TestConnectionCheck)
#include "tst_connectioncheck.moc"
