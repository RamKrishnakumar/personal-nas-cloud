#include "core/Tls/CertRenewer.h"
#include "core/Tls/Tailscale.h"

#include "fixtures/NoTailscaleOnPath.h"

#include <QFile>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

// ── The certificate that caused a real failure ──────────────────────────────
// A faithful copy of what scripts/gen-cert.sh had written on the development
// machine: CN=nestdrive.local, SANs covering localhost, the LAN address and the DDNS
// hostname, and valid for years. Nothing in it names a .ts.net host.
//
// Reproducing it exactly matters, because the shape is the bug. A renewer that asks
// only "is this close to expiry?" sees a certificate good until well past 2100,
// concludes there is nothing to do, and never requests the real one — so the server
// keeps presenting this to a browser reaching it by a .ts.net name, and the browser
// refuses it. Which is what an iPhone reported.
constexpr const char* StaleCertificate = R"(-----BEGIN CERTIFICATE-----
MIIB3zCCAYWgAwIBAgIUZlbuo3qmuU3mz1nHtohx2vjtG8UwCgYIKoZIzj0EAwIw
OzEYMBYGA1UEAwwPbmVzdGRyaXZlLmxvY2FsMRIwEAYDVQQKDAlOZXN0RHJpdmUx
CzAJBgNVBAYTAklOMCAXDTI2MTAyMzE4MDEzOFoYDzIxMjYwOTI5MTgwMTM4WjA7
MRgwFgYDVQQDDA9uZXN0ZHJpdmUubG9jYWwxEjAQBgNVBAoMCU5lc3REcml2ZTEL
MAkGA1UEBhMCSU4wWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAASMjmE6wAFvmpOt
cAhG18pQcPrjbx6sSEB/5RK9ki3+FTTAAoQyZXjh1EqtI1TOpk5S1Ro/1LTGlVYU
3CwoLAeqo2UwYzA0BgNVHREELTArgglsb2NhbGhvc3SHBH8AAAGHBMCoAIWCEm5l
c3Rkcml2ZS5kZG5zLm5ldDAMBgNVHRMBAf8EAjAAMB0GA1UdDgQWBBRrxAvG1XRY
r7ZgJ1BHGMY5lvSnxTAKBggqhkjOPQQDAgNIADBFAiBhNIaBhe6zeiiRdxGgYsoV
TEA5eBkRcN+6JizQ98dHVQIhAJX3qOkwmuRsbB1zxJLSgWra+3QFgSyO9bn8joIE
isay
-----END CERTIFICATE-----
)";

constexpr const char* StaleKey = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIC3cNDvdxyME3p8EzWyYMFCQx0xIJt5m9Bhy4mwxKLa9oAoGCCqGSM49
AwEHoUQDQgAEjI5hOsABb5qTrXAIRtfKUHD6428erEhAf+USvZIt/hU0wAKEMmV4
4dRKrSNUzqZOUtUaP9S0xpVWFNwsKCwHqg==
-----END EC PRIVATE KEY-----
)";

// ── A certificate with no subjectAltName at all ─────────────────────────────
// Its common name is exactly the name we want, which makes it a trap rather than a
// formality: code that fell back to the CN when no SAN was present would pass this
// test while disagreeing with every browser written in the last decade.
constexpr const char* CommonNameOnlyCertificate = R"(-----BEGIN CERTIFICATE-----
MIIBcTCCARegAwIBAgIUDREW3IGqlUvABZAD9ZzYRLDOleEwCgYIKoZIzj0EAwIw
JjEkMCIGA1UEAwwbbmVzdGRyaXZlLnRhaWwxYTJiM2MudHMubmV0MCAXDTI2MTAy
MzE4MDEzOVoYDzIxMjYwOTI5MTgwMTM5WjAmMSQwIgYDVQQDDBtuZXN0ZHJpdmUu
dGFpbDFhMmIzYy50cy5uZXQwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQ96HVd
0rOViCuGNooiO9Q/yOTPrmdI/pEct/68zrGlrvrzdLKIYsXnyZbsHOuFZob0KKJz
Z4JLU1JNgqs2fpH8oyEwHzAdBgNVHQ4EFgQUNv8F6beCNTsIvstxKVQv523uA6cw
CgYIKoZIzj0EAwIDSAAwRQIgFJLK20jB0pWkS/Pew6AbpI5XzSwVoR3JBoFNFKsL
k98CIQCpUcFEKQdFN4m+4G31b0dcYWLVBQYb99q/C9NanGPECQ==
-----END CERTIFICATE-----
)";

/// SAN is exactly `*.wild.example.com`.
constexpr const char* WildcardCertificate = R"(-----BEGIN CERTIFICATE-----
MIIBajCCARCgAwIBAgIUR8Etc8GwFvUlBx3Ygh80K6Ih7nQwCgYIKoZIzj0EAwIw
EzERMA8GA1UEAwwId2lsZGNhcmQwIBcNMjYxMDIzMTgwMTM5WhgPMjEyNjA5Mjkx
ODAxMzlaMBMxETAPBgNVBAMMCHdpbGRjYXJkMFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAEebIM2GkpI4rqq9gIuZi3+tObpFXebIxED+frMgQr8I4G8PSuBoKouy43
NpOvK+vPvUr+F9tYnn30ORazLLbYXaNAMD4wHQYDVR0RBBYwFIISKi53aWxkLmV4
YW1wbGUuY29tMB0GA1UdDgQWBBT6x5GMsfn6ycPKeU6FCFKm5OE4oTAKBggqhkjO
PQQDAgNIADBFAiEAos651gotVQ9TPB2HNuhaexB+nn1qSaq9fCHeSOAnv+ICIHwT
Afb0RJbAae7xwDdq7oV9ckeW0D9yFgCp4819xLQd
-----END CERTIFICATE-----
)";

/// The MagicDNS name the fake tailscale fixture reports, and issues for.
///
/// A view rather than a `const char*` so it can be passed straight to anything
/// taking a QString: QStringLiteral is a macro that measures its argument at compile
/// time and therefore only accepts an actual string literal.
constexpr QLatin1StringView WantedName("nestdrive.tail1a2b3c.ts.net");

QSslCertificate certificateFrom(const char* pem)
{
    const QList<QSslCertificate> parsed =
        QSslCertificate::fromData(QByteArray(pem), QSsl::Pem);

    return parsed.isEmpty() ? QSslCertificate() : parsed.first();
}

} // namespace

/**
 * @file
 * @brief Coverage for core::CertRenewer.
 *
 * Two halves, deliberately separated. shouldRenew() is the only decision in the
 * class and is a pure function of two instants, so it is tested exhaustively with
 * QDateTimes and no certificate authority anywhere near it. The rest is plumbing —
 * ask the CLI, reload the result, announce it — and is tested through the fake
 * tailscale fixture.
 */
class TestCertRenewer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ── Policy ────────────────────────────────────────────────────────────
    void doesNotRenewALongLivedCertificate();
    void renewsInsideTheThreshold();
    void renewsExactlyAtTheThreshold();
    void renewsAnExpiredCertificate();
    void renewsWhenTheExpiryIsUnknown();
    void fallsBackToTheDefaultThreshold_data();
    void fallsBackToTheDefaultThreshold();

    // ── Name coverage ─────────────────────────────────────────────────────
    void coversAnExactSubjectAlternativeName();
    void coversRegardlessOfCase();
    void doesNotCoverADifferentName();
    void doesNotCoverOnTheStrengthOfACommonName();
    void coversOneLabelUnderAWildcard();
    void doesNotCoverTheBareDomainOfAWildcard();
    void doesNotCoverTwoLabelsUnderAWildcard();
    void coversNothingWhenAskedAboutNothing();

    // ── Plumbing ──────────────────────────────────────────────────────────
    void renewsWhenTheCertificateIsForAnotherName();
    void doesNothingWithoutTailscale();
    void reportsBeingLoggedOut();
    void renewsAndAnnouncesTheNewCertificate();
    void reportsExpiryOfTheLeaf();
    void doesNotRenewTwiceOnceTheCertificateIsFresh();
    void reportsAFailureWithoutAnnouncingAnything();

private:
    std::unique_ptr<core::Tailscale> fake(const QByteArray& mode = "ok");

    QString certPath() const { return m_tmp->path() + QStringLiteral("/live.crt"); }
    QString keyPath()  const { return m_tmp->path() + QStringLiteral("/live.key"); }

    /** Puts a certificate and key on disk where the renewer will find them. */
    bool seed(const char* certificatePem, const char* keyPem);

    std::unique_ptr<QTemporaryDir> m_tmp;
};

void TestCertRenewer::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
}

void TestCertRenewer::cleanup()
{
    qunsetenv("FAKE_TAILSCALE_MODE");
    m_tmp.reset();
}

std::unique_ptr<core::Tailscale> TestCertRenewer::fake(const QByteArray& mode)
{
    qputenv("FAKE_TAILSCALE_MODE", mode);
    return std::make_unique<core::Tailscale>(QStringLiteral(FAKE_TAILSCALE_PATH));
}

bool TestCertRenewer::seed(const char* certificatePem, const char* keyPem)
{
    const auto write = [](const QString& path, const char* contents) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;

        file.write(contents);
        return true;
    };

    return write(certPath(), certificatePem) && write(keyPath(), keyPem);
}

// ── Policy ──────────────────────────────────────────────────────────────────

void TestCertRenewer::doesNotRenewALongLivedCertificate()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // A certificate issued yesterday has about 89 days left. Nothing to do, and
    // doing nothing has to be the common case: this check runs daily.
    QVERIFY(!core::CertRenewer::shouldRenew(now.addDays(89), now, 14));
}

void TestCertRenewer::renewsInsideTheThreshold()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(core::CertRenewer::shouldRenew(now.addDays(5), now, 14));
}

void TestCertRenewer::renewsExactlyAtTheThreshold()
{
    // The boundary, pinned because off-by-one here means either renewing a day early
    // forever or missing the window by a day — and the second one expires a
    // certificate on a server nobody is watching.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(core::CertRenewer::shouldRenew(now.addDays(14), now, 14));

    // One second past the threshold is outside it.
    QVERIFY(!core::CertRenewer::shouldRenew(now.addDays(14).addSecs(1), now, 14));
}

void TestCertRenewer::renewsAnExpiredCertificate()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(core::CertRenewer::shouldRenew(now.addDays(-1), now, 14));
}

void TestCertRenewer::renewsWhenTheExpiryIsUnknown()
{
    // An invalid expiry means the certificate could not be read or carries no date.
    // Renewing is the right answer: replacing something we cannot reason about beats
    // serving it until a browser refuses it, and being wrong costs one CLI call that
    // returns the cached certificate anyway.
    QVERIFY(core::CertRenewer::shouldRenew(QDateTime(),
                                           QDateTime::currentDateTimeUtc(),
                                           14));
}

void TestCertRenewer::fallsBackToTheDefaultThreshold_data()
{
    QTest::addColumn<int>("threshold");

    QTest::newRow("zero")     << 0;
    QTest::newRow("negative") << -30;
}

void TestCertRenewer::fallsBackToTheDefaultThreshold()
{
    QFETCH(int, threshold);

    const QDateTime now = QDateTime::currentDateTimeUtc();

    // A nonsensical certRenewDays in config.ini must not switch renewal off. Taken
    // literally, zero would mean "renew only once it has already expired", which is
    // a configuration mistake that silently breaks HTTPS months later.
    QVERIFY(core::CertRenewer::shouldRenew(now.addDays(3), now, threshold));
    QVERIFY(!core::CertRenewer::shouldRenew(now.addDays(60), now, threshold));
}

// ── Name coverage ───────────────────────────────────────────────────────────

void TestCertRenewer::coversAnExactSubjectAlternativeName()
{
    // The self-signed certificate lists four names; nestdrive.ddns.net is one of
    // them, and matching it is the straightforward case everything else is measured
    // against.
    const QSslCertificate stale = certificateFrom(StaleCertificate);

    QVERIFY(!stale.isNull());
    QVERIFY(core::CertRenewer::covers(stale,
                                      QStringLiteral("nestdrive.ddns.net")));
    QVERIFY(core::CertRenewer::covers(stale, QStringLiteral("localhost")));
}

void TestCertRenewer::coversRegardlessOfCase()
{
    // DNS is case-insensitive, and a name can reach this code from JSON, a config
    // file or a URL — any of which may differ in case from the certificate.
    const QSslCertificate stale = certificateFrom(StaleCertificate);

    QVERIFY(core::CertRenewer::covers(stale,
                                      QStringLiteral("NestDrive.DDNS.net")));
}

void TestCertRenewer::doesNotCoverADifferentName()
{
    // The heart of the reported bug, as a one-line assertion.
    const QSslCertificate stale = certificateFrom(StaleCertificate);

    QVERIFY(!stale.isNull());
    QVERIFY2(!core::CertRenewer::covers(stale, WantedName),
             "the certificate that caused the iPhone warning must not be "
             "considered valid for the MagicDNS name");

    // It does cover the names it actually lists, so this is not simply failing.
    QVERIFY(core::CertRenewer::covers(stale,
                                      QStringLiteral("nestdrive.ddns.net")));
}

void TestCertRenewer::doesNotCoverOnTheStrengthOfACommonName()
{
    // This certificate's common name IS the name being asked about, and it has no
    // SAN extension at all — so it is a trap for exactly the convenience that would
    // feel reasonable to add. Browsers stopped honouring the common name years ago;
    // accepting it here would mean believing a certificate is fine while Safari
    // rejects it, which is the failure mode this whole test file exists for.
    const QSslCertificate cnOnly = certificateFrom(CommonNameOnlyCertificate);

    QVERIFY(!cnOnly.isNull());
    QCOMPARE(cnOnly.subjectInfo(QSslCertificate::CommonName).value(0),
             QString(WantedName));
    QVERIFY(cnOnly.subjectAlternativeNames().isEmpty());

    QVERIFY(!core::CertRenewer::covers(cnOnly, WantedName));
}

void TestCertRenewer::coversOneLabelUnderAWildcard()
{
    const QSslCertificate wild = certificateFrom(WildcardCertificate);

    QVERIFY(!wild.isNull());
    QVERIFY(core::CertRenewer::covers(wild,
                                      QStringLiteral("nas.wild.example.com")));
}

void TestCertRenewer::doesNotCoverTheBareDomainOfAWildcard()
{
    // *.example.com does not match example.com. Browsers agree, and a suffix
    // comparison alone would get this wrong.
    const QSslCertificate wild = certificateFrom(WildcardCertificate);

    QVERIFY(!core::CertRenewer::covers(wild,
                                       QStringLiteral("wild.example.com")));
}

void TestCertRenewer::doesNotCoverTwoLabelsUnderAWildcard()
{
    // A wildcard covers exactly one label. This is the case a naive endsWith() check
    // accepts, and believing a certificate covers a name it does not is worse than
    // re-requesting one unnecessarily: it produces the browser warning rather than a
    // wasted call.
    const QSslCertificate wild = certificateFrom(WildcardCertificate);

    QVERIFY(!core::CertRenewer::covers(wild,
                                       QStringLiteral("a.b.wild.example.com")));
}

void TestCertRenewer::coversNothingWhenAskedAboutNothing()
{
    const QSslCertificate stale = certificateFrom(StaleCertificate);

    QVERIFY(!core::CertRenewer::covers(stale, QString()));
    QVERIFY(!core::CertRenewer::covers(QSslCertificate(),
                                       WantedName));
}

// ── Plumbing ────────────────────────────────────────────────────────────────

void TestCertRenewer::renewsWhenTheCertificateIsForAnotherName()
{
    // The regression test for the bug a real iPhone found.
    //
    // A valid, long-lived certificate is already on disk — which is the state of
    // every existing installation, because scripts/gen-cert.sh wrote one. It is for
    // the wrong names. The renewer used to look only at the expiry date, see a
    // certificate good for another century, and do nothing at all, silently.
    QVERIFY(seed(StaleCertificate, StaleKey));

    const QDateTime before = QDateTime::currentDateTimeUtc();
    QVERIFY2(core::CertRenewer::shouldRenew(
                 certificateFrom(StaleCertificate).expiryDate(), before, 14)
                 == false,
             "the seeded certificate must NOT look due on dates alone, or this "
             "test would pass for the wrong reason");

    const auto tailscale = fake();
    core::CertRenewer renewer(tailscale.get(), certPath(), keyPath());
    renewer.setDnsName(WantedName);

    QSignalSpy renewed(&renewer, &core::CertRenewer::certificateRenewed);

    QVERIFY2(renewer.checkNow(), qPrintable(renewer.lastError()));
    QCOMPARE(renewed.count(), 1);

    // And what replaced it actually covers the name this time.
    QVERIFY(core::CertRenewer::covers(renewer.currentLeaf(),
                                      WantedName));
}

void TestCertRenewer::doesNothingWithoutTailscale()
{
    // No Tailscale is an ordinary configuration, not a fault: the server runs on the
    // LAN with its self-signed certificate. So this reports nothing rather than
    // raising anything.
    //
    // See fixtures/NoTailscaleOnPath.h for why the guard is needed to ask this at
    // all on a machine that does have Tailscale installed.
    const NoTailscaleOnPath guard;

    core::Tailscale absent(m_tmp->path() + QStringLiteral("/not-here.exe"));
    core::CertRenewer renewer(&absent, certPath(), keyPath());
    renewer.setDnsName(QStringLiteral("nestdrive.tail1a2b3c.ts.net"));

    QSignalSpy renewed(&renewer, &core::CertRenewer::certificateRenewed);
    QSignalSpy failed(&renewer, &core::CertRenewer::renewalFailed);

    QVERIFY(!renewer.checkNow());
    QCOMPARE(renewed.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void TestCertRenewer::reportsBeingLoggedOut()
{
    // Installed but logged out is worth a message, unlike not installed at all: the
    // owner has started setting this up and needs to be told what is missing.
    const auto tailscale = fake();
    core::CertRenewer renewer(tailscale.get(), certPath(), keyPath());

    QVERIFY(!renewer.checkNow());
    QVERIFY2(renewer.lastError().contains(QStringLiteral("logged out")),
             qPrintable(renewer.lastError()));
}

void TestCertRenewer::renewsAndAnnouncesTheNewCertificate()
{
    const auto tailscale = fake();
    core::CertRenewer renewer(tailscale.get(), certPath(), keyPath());
    renewer.setDnsName(QStringLiteral("nestdrive.tail1a2b3c.ts.net"));

    QSignalSpy renewed(&renewer, &core::CertRenewer::certificateRenewed);

    // No certificate on disk at all, so the expiry is unknown and renewal is due.
    QVERIFY2(renewer.checkNow(), qPrintable(renewer.lastError()));
    QCOMPARE(renewed.count(), 1);

    // The signal carries something the server can actually serve. This is the whole
    // contract: FileServer hands it straight to HttpServer, so a configuration that
    // did not parse would break every new connection with no warning page to click
    // through — strictly worse than the expired certificate it replaced.
    const auto configuration =
        renewed.at(0).at(0).value<QSslConfiguration>();

    QCOMPARE(configuration.localCertificateChain().size(), 2);
    QVERIFY(!configuration.privateKey().isNull());
    QCOMPARE(configuration.privateKey().algorithm(), QSsl::Ec);
}

void TestCertRenewer::reportsExpiryOfTheLeaf()
{
    const auto tailscale = fake();
    core::CertRenewer renewer(tailscale.get(), certPath(), keyPath());
    renewer.setDnsName(QStringLiteral("nestdrive.tail1a2b3c.ts.net"));

    QVERIFY(!renewer.currentExpiry().isValid());
    QVERIFY2(renewer.checkNow(), qPrintable(renewer.lastError()));

    // The leaf's expiry, not the intermediate's. An intermediate outliving the leaf
    // is normal, and timing renewal against it would wait far too long.
    const QList<QSslCertificate> chain =
        QSslCertificate::fromPath(certPath(), QSsl::Pem);
    QCOMPARE(chain.size(), 2);
    QCOMPARE(renewer.currentExpiry(), chain.first().expiryDate());
}

void TestCertRenewer::doesNotRenewTwiceOnceTheCertificateIsFresh()
{
    // The daily check must be a no-op almost every time it runs. The fixture's
    // certificate is valid for a century, so the second call has nothing to do.
    //
    // This now also guards the other side of the name test added for the
    // wrong-certificate bug: the fixture issues for the same name it reports, so a
    // renewer that compared names too strictly — or compared the wrong pair of them
    // — would request a new certificate on every check instead of once.
    const auto tailscale = fake();
    core::CertRenewer renewer(tailscale.get(), certPath(), keyPath());
    renewer.setDnsName(QStringLiteral("nestdrive.tail1a2b3c.ts.net"));

    QVERIFY2(renewer.checkNow(), qPrintable(renewer.lastError()));

    QSignalSpy renewed(&renewer, &core::CertRenewer::certificateRenewed);
    QVERIFY(!renewer.checkNow());
    QCOMPARE(renewed.count(), 0);
}

void TestCertRenewer::reportsAFailureWithoutAnnouncingAnything()
{
    const auto tailscale = fake("no_https");
    core::CertRenewer renewer(tailscale.get(), certPath(), keyPath());
    renewer.setDnsName(QStringLiteral("nestdrive.tail1a2b3c.ts.net"));

    QSignalSpy renewed(&renewer, &core::CertRenewer::certificateRenewed);
    QSignalSpy failed(&renewer, &core::CertRenewer::renewalFailed);

    QVERIFY(!renewer.checkNow());

    QCOMPARE(failed.count(), 1);
    QCOMPARE(renewed.count(), 0);

    QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("HTTPS must be enabled")),
             qPrintable(failed.at(0).at(0).toString()));
}

QTEST_MAIN(TestCertRenewer)
#include "tst_certrenewer.moc"
