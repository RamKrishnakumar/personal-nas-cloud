#include "core/Remote/RemoteMethod.h"

#include <QtTest>

/**
 * @file
 * @brief Coverage for core::resolveRemoteMethod().
 *
 * Small, pure, and with more combinations than it looks: three configured values
 * against three connection types against two booleans. The interesting ones are
 * where what was asked for is not available, because silently doing something else
 * is how an owner ends up with no remote access and no idea why.
 */
class TestRemoteMethod : public QObject
{
    Q_OBJECT

private slots:
    void automaticPrefersThePublicLink();
    void automaticFallsBackToTailscaleBehindCarrierNat();
    void automaticWillNotGuessPublicOnAnUnknownConnection();
    void automaticNeedsAHostnameForThePublicLink();
    void automaticEndsAtLanOnlyWithNothingAvailable();

    void explicitPublicIsHonouredWhenPossible();
    void explicitPublicFallsBackWhenImpossible();
    void explicitPublicFallsToLanWhenTailscaleIsAlsoUnavailable();

    void explicitTailscaleIgnoresAPublicAddress();
    void explicitTailscaleFallsBackWhenSignedOut();

    void lanOnlyIsAlwaysRespected();

    void roundTripsThroughConfiguration_data();
    void roundTripsThroughConfiguration();
    void unrecognisedConfigurationReadsAsAutomatic();
    void everyMethodHasADescription();
};

void TestRemoteMethod::automaticPrefersThePublicLink()
{
    // On the BSNL connection, with a hostname configured and Tailscale also running,
    // the public link wins — it is the only one of the two somebody can open without
    // installing anything, which is worth more than not configuring a router.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Auto,
                                       core::ConnectionType::PublicIp,
                                       true,
                                       true),
             core::RemoteMethod::Public);
}

void TestRemoteMethod::automaticFallsBackToTailscaleBehindCarrierNat()
{
    // The Airtel connection. A forwarded port cannot work, so the overlay is the
    // only option — and it does work there.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Auto,
                                       core::ConnectionType::CarrierNat,
                                       true,
                                       true),
             core::RemoteMethod::Tailscale);
}

void TestRemoteMethod::automaticWillNotGuessPublicOnAnUnknownConnection()
{
    // Unknown means the router would not say and the address did not settle it.
    // Guessing towards Public there produces a link that times out with no
    // explanation; guessing towards Tailscale produces one that works.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Auto,
                                       core::ConnectionType::Unknown,
                                       true,
                                       true),
             core::RemoteMethod::Tailscale);
}

void TestRemoteMethod::automaticNeedsAHostnameForThePublicLink()
{
    // A public address with no DDNS hostname leaves only a bare number to hand out,
    // which no certificate will match — so it is not usable as the public path even
    // though the connection could carry it.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Auto,
                                       core::ConnectionType::PublicIp,
                                       true,
                                       false),
             core::RemoteMethod::Tailscale);
}

void TestRemoteMethod::automaticEndsAtLanOnlyWithNothingAvailable()
{
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Auto,
                                       core::ConnectionType::CarrierNat,
                                       false,
                                       false),
             core::RemoteMethod::LanOnly);
}

void TestRemoteMethod::explicitPublicIsHonouredWhenPossible()
{
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Public,
                                       core::ConnectionType::PublicIp,
                                       true,
                                       true),
             core::RemoteMethod::Public);
}

void TestRemoteMethod::explicitPublicFallsBackWhenImpossible()
{
    // Asked for on a connection behind the provider's NAT. There is nothing to
    // honour — a rule on that router has nothing on the internet able to match it —
    // so this falls back rather than pretending. The caller is expected to say so;
    // see MainWindow's remote access dialog.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Public,
                                       core::ConnectionType::CarrierNat,
                                       true,
                                       true),
             core::RemoteMethod::Tailscale);

    // Same for a connection we could not classify: not confirmed is not good enough
    // to hand somebody a link with.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Public,
                                       core::ConnectionType::Unknown,
                                       true,
                                       true),
             core::RemoteMethod::Tailscale);
}

void TestRemoteMethod::explicitPublicFallsToLanWhenTailscaleIsAlsoUnavailable()
{
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Public,
                                       core::ConnectionType::CarrierNat,
                                       false,
                                       true),
             core::RemoteMethod::LanOnly);
}

void TestRemoteMethod::explicitTailscaleIgnoresAPublicAddress()
{
    // An explicit choice is not second-guessed when it can be satisfied. Somebody on
    // a public connection may well prefer the overlay — nothing is exposed publicly
    // and no router rule is needed.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Tailscale,
                                       core::ConnectionType::PublicIp,
                                       true,
                                       true),
             core::RemoteMethod::Tailscale);
}

void TestRemoteMethod::explicitTailscaleFallsBackWhenSignedOut()
{
    // Note it does NOT fall through to the public link. Choosing Tailscale is often
    // a privacy decision, and quietly publishing the library on a forwarded port
    // instead would be the opposite of what was asked for.
    QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::Tailscale,
                                       core::ConnectionType::PublicIp,
                                       false,
                                       true),
             core::RemoteMethod::LanOnly);
}

void TestRemoteMethod::lanOnlyIsAlwaysRespected()
{
    // Whatever is available, this is somebody saying they do not want the library
    // reachable from outside. There is no combination that should override it.
    for (const auto connection : { core::ConnectionType::Unknown,
                                   core::ConnectionType::PublicIp,
                                   core::ConnectionType::CarrierNat }) {
        for (const bool online : { false, true }) {
            for (const bool hostname : { false, true }) {
                QCOMPARE(core::resolveRemoteMethod(core::RemoteMethod::LanOnly,
                                                   connection, online, hostname),
                         core::RemoteMethod::LanOnly);
            }
        }
    }
}

void TestRemoteMethod::roundTripsThroughConfiguration_data()
{
    QTest::addColumn<int>("method");

    QTest::newRow("auto")      << int(core::RemoteMethod::Auto);
    QTest::newRow("tailscale") << int(core::RemoteMethod::Tailscale);
    QTest::newRow("public")    << int(core::RemoteMethod::Public);
    QTest::newRow("lan")       << int(core::RemoteMethod::LanOnly);
}

void TestRemoteMethod::roundTripsThroughConfiguration()
{
    QFETCH(int, method);
    const auto original = core::RemoteMethod(method);

    QCOMPARE(core::remoteMethodFromString(core::remoteMethodToString(original)),
             original);
}

void TestRemoteMethod::unrecognisedConfigurationReadsAsAutomatic()
{
    QCOMPARE(core::remoteMethodFromString(QStringLiteral("wireguard")),
             core::RemoteMethod::Auto);
    QCOMPARE(core::remoteMethodFromString(QString()), core::RemoteMethod::Auto);
    QCOMPARE(core::remoteMethodFromString(QStringLiteral(" TailScale ")),
             core::RemoteMethod::Tailscale);
}

void TestRemoteMethod::everyMethodHasADescription()
{
    for (const auto method : { core::RemoteMethod::Auto,
                               core::RemoteMethod::Tailscale,
                               core::RemoteMethod::Public,
                               core::RemoteMethod::LanOnly }) {
        QVERIFY(!core::describeRemoteMethod(method).isEmpty());
    }
}

QTEST_MAIN(TestRemoteMethod)
#include "tst_remotemethod.moc"
