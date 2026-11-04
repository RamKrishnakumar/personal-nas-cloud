#include "core/Tls/TlsConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

/**
 * @file
 * @brief Coverage for core::loadTls().
 *
 * The first tests this code has ever had. It spent v0.5 and v0.6 compiled only into
 * the application executable, where no suite could link it, and accumulated two
 * defects that are invisible to the one certificate it was ever given — the
 * single-file RSA pair that scripts/gen-cert.sh writes:
 *
 *   - the key algorithm was hardcoded to RSA, rejecting the ECDSA keys that
 *     `tailscale cert` and modern ACME clients issue;
 *   - only the first certificate in the file was read, discarding the intermediates
 *     that a real authority sends and that Safari and most Android clients need.
 *
 * Both are the sort of thing that works perfectly until the day it matters, so the
 * tests below are deliberately written around certificate *shapes* rather than
 * around the one shape in use at the time.
 *
 * ### Why the PEM is in the source
 *
 * .gitignore excludes *.pem, *.key and *.crt across the whole repository, which is
 * correct — a committed private key is a committed private key even in a test — but
 * it means fixture files would pass here and be missing from a fresh clone. The
 * material below is therefore embedded and written to a QTemporaryDir per test.
 *
 * These keys are published in a public repository and are worthless by construction.
 * Their certificates are self-signed or signed by a throwaway intermediate, carry
 * names nobody owns, and are never trusted by anything. Validity runs to 2126 so the
 * suite cannot start failing on a date; nothing here validates a chain, so expiry is
 * irrelevant to what is being tested anyway.
 */

namespace {

// ── EC P-256, the shape `tailscale cert` produces ───────────────────────────
// One certificate, and the same key in both encodings openssl emits: SEC1 under a
// "BEGIN EC PRIVATE KEY" banner, and PKCS#8 under the generic "BEGIN PRIVATE KEY".
// Both must load. The generic banner is the interesting one, because its header says
// nothing at all about the algorithm inside.

constexpr const char* EcCertificate = R"(-----BEGIN CERTIFICATE-----
MIIBwzCCAWmgAwIBAgIUVtL42BQOSWeSuDNhAWkd5JGZmngwCgYIKoZIzj0EAwIw
IzEhMB8GA1UEAwwYbmVzdGRyaXZlLmV4YW1wbGUudHMubmV0MCAXDTI2MTAyMjE5
MzkwNFoYDzIxMjYwOTI4MTkzOTA0WjAjMSEwHwYDVQQDDBhuZXN0ZHJpdmUuZXhh
bXBsZS50cy5uZXQwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQEqYpWIsItKivj
jrkWbbbLuRwI3AZAz5xgfR9Rbbe68cB+YBDhzFDMHdAJuvUyUYbA8+MrgI10Sjmf
lwkzBnZNo3kwdzAjBgNVHREEHDAaghhuZXN0ZHJpdmUuZXhhbXBsZS50cy5uZXQw
DAYDVR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCBaAwEwYDVR0lBAwwCgYIKwYBBQUH
AwEwHQYDVR0OBBYEFCzk8l+fpI+JO8nzrERCXJkHWQxxMAoGCCqGSM49BAMCA0gA
MEUCIQDRShzvyyyxO96Ql8vD2jR532YFQ5aiMgLzq4Tf2qgaTAIgLrPJAFJOlrwN
rLqGK0NCYEVTmb19fmxWJbllHBqrgUY=
-----END CERTIFICATE-----
)";

constexpr const char* EcKeySec1 = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIO3wWgy31QZ3RqRFXuOXXKxfpFJcyhd5sW6Ce577sgZpoAoGCCqGSM49
AwEHoUQDQgAEBKmKViLCLSor4465Fm22y7kcCNwGQM+cYH0fUW23uvHAfmAQ4cxQ
zB3QCbr1MlGGwPPjK4CNdEo5n5cJMwZ2TQ==
-----END EC PRIVATE KEY-----
)";

constexpr const char* EcKeyPkcs8 = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg7fBaDLfVBndGpEVe
45dcrF+kUlzKF3mxboJ7nvuyBmmhRANCAAQEqYpWIsItKivjjrkWbbbLuRwI3AZA
z5xgfR9Rbbe68cB+YBDhzFDMHdAJuvUyUYbA8+MrgI10SjmflwkzBnZN
-----END PRIVATE KEY-----
)";

// ── RSA 2048, the shape scripts/gen-cert.sh produces ────────────────────────
// The case that already worked. Present so that teaching the loader about EC cannot
// quietly cost us the LAN setup everybody is already using.

constexpr const char* RsaCertificate = R"(-----BEGIN CERTIFICATE-----
MIIDTzCCAjegAwIBAgIUSyxEa7DNkQsbihJynDOcdcOs8Z0wDQYJKoZIhvcNAQEL
BQAwIzEhMB8GA1UEAwwYbmVzdGRyaXZlLmV4YW1wbGUudHMubmV0MCAXDTI2MTAy
MjE5MzkwNFoYDzIxMjYwOTI4MTkzOTA0WjAjMSEwHwYDVQQDDBhuZXN0ZHJpdmUu
ZXhhbXBsZS50cy5uZXQwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCp
P93lEDi+AQ9XHw3qBOpQccqbnEQ5px392sSogDZw8wn65aKGzq/1C1aZdOEbOOrB
zB2lyxWlmlpXkMAJ2IG+0+l48omw+arrdAkayUYV8md6lyQBT/2nCM9RHfg01Uo8
iW4IAjJiUu2Z3X/yF+G1c0xS2Ahl4MF/kMvhzntCYjUF5Q6xaFq+xWuMqPWRdUAT
hkchNFyNtMppe2d/CAqhEjMggYXxJg5jzLMPP6lOn98yDdhU3b9G5nWo8XGrRtqI
TQ1OrHHNW7EtKM6mXrrwoe5hAM8t0PHYnIy1DAbXZEqZ6xfKG0qqeYC/UD2CpotO
5WpkhpcexzMU5QeHXi3FAgMBAAGjeTB3MCMGA1UdEQQcMBqCGG5lc3Rkcml2ZS5l
eGFtcGxlLnRzLm5ldDAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIFoDATBgNV
HSUEDDAKBggrBgEFBQcDATAdBgNVHQ4EFgQUelHRy/GIJaXX+JqxucafGRSU4oAw
DQYJKoZIhvcNAQELBQADggEBAELEgGDA6t140fstRybTBei+X362ZmnWKZj4DFPJ
LS8E3eQc2R7lagGQrdLqEPcv6ole2MlIW1RxMcn36V8aXTo7YcQSTRKW7elPDBC3
yV4d7PXxmRI/JgBXbCH6Y19OOMVt2vyp4yrw0NRk9u/9U24ePxyHSnsFE3J+Bdos
4KL0M8+1MY7U/XAfB67IURrLzK4wR9WDAbbnJyNFgCwXo7E4LeVZpFQbkgoqVS4U
oA7YTFiKTf5h4l5drOh9eSAkKECwtgz8pqAwWiHQJ+vWGx7F1M3tkx92ClUUxaTA
XdOWhmq/ZnT9/Pfr5WtTJWnvDupAdNmVrKHA/Ls9v7yMYGg=
-----END CERTIFICATE-----
)";

constexpr const char* RsaKey = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCpP93lEDi+AQ9X
Hw3qBOpQccqbnEQ5px392sSogDZw8wn65aKGzq/1C1aZdOEbOOrBzB2lyxWlmlpX
kMAJ2IG+0+l48omw+arrdAkayUYV8md6lyQBT/2nCM9RHfg01Uo8iW4IAjJiUu2Z
3X/yF+G1c0xS2Ahl4MF/kMvhzntCYjUF5Q6xaFq+xWuMqPWRdUAThkchNFyNtMpp
e2d/CAqhEjMggYXxJg5jzLMPP6lOn98yDdhU3b9G5nWo8XGrRtqITQ1OrHHNW7Et
KM6mXrrwoe5hAM8t0PHYnIy1DAbXZEqZ6xfKG0qqeYC/UD2CpotO5WpkhpcexzMU
5QeHXi3FAgMBAAECggEAD4y9arHTPl6DXEvCgxrkgQ0s9WylnNTMndAfSIpPD8Px
YzD9F7AE3PMOIWQEDCfUjO2fihkrtsVRqFq1ahiPaX4d8aDG8CpgGIKje4FWhEzh
hcY95pwrMwRNZKe0o2LQdgz78PxsSKBTeB/jnqIXEkBMZagd5wF/uNd2A+igM0Vp
8Tu+G+3tpIrcSpAKj4cXXgByJxhARL2RAKCOnPjsNU/7Y2g9V8/NpQtknhffQc5y
K+q5C3lNIx8k17NPO1FEa5IFy9As13rEXxFagi+NKJUlQ7cxYkQGZ+CuCpXV/mZm
eIYDNnHvmRQuXViMOVUrBLH2blCo1xlcew7RjStfmQKBgQDQRkEsXJcXaT8jXxMe
0UryzqTpzwOrURnSBszA79NDl6kQrH8HZV64pqPg1aInWRDNBvVXNNSt0YE3L1eJ
/g9GdL/O1eP7VjDZzleJ3eZKMqiZ5cikPtTvhuZ3T5W7CI8IS88yO0p7xBwT06s/
RuVcqJ4WXvEpUVnRRz0hR2D1hwKBgQDQCFb1Ecr9IL+tfmdHMw85K/2bG8TdGwY9
3bn+BvgdqIQg1FnO5l/990cf0FuVcgaPY6wuwQFFLLKKW0FHMWNUmzHCMdmPfzGv
LIG2gja4icSQqj16YJRXqY4axIRetY/AB4uOXNo+udni4Za+LLrKMo7lIIs1uZsl
yUxGrYGVUwKBgQCqKTwGKn+nOO3E0GCmxQge6IYEIIY26cPWvId8+nP4vWf7n3MJ
b+C7rCon/2LEExiO6ih4X0mD2a9gjuVOcKu6PJiZPFdPeVPgH52Vnh4QcOMfGYWb
61m9eNHCmNsUPCHf/VADEj4AcHEgIce/owSxd3HxhRAzWauwOCvRjKfC8QKBgEtT
WbkvEiMJop39cQP01wRKi2+IZIXy7RWlnVz/xQNoKz9/5GzoOcXJrbU1SpQYT4vH
+WaMeBtGDffPBxnyJhdSgnfgePheg+z73pPpHks/hy8ilVQkeFItkMjcc1mfwEc1
9DA+KsAfp2dha6W8qH3iRJ9uYzm21RifhMDqvksFAoGAPSTAUYlX3MPmH/FWGMs6
ibOBI1TouJhqDqooBadi/RIs3sFd9NHLOb+DlOO0TsDMY0Wq7ayP8Nz8guvENm5C
iUvxd6zbZHg4FCRlxvyTE+mTwGbB4PEdaY9/MwMGtDtx9d92iyhgAReHJG+c7Q9N
2YWXnuL3tff+ThJooQEfFCk=
-----END PRIVATE KEY-----
)";

// ── A real two-certificate chain ────────────────────────────────────────────
// Leaf first, then the intermediate that signed it — the order every certificate
// authority writes and the order setLocalCertificateChain() expects. Genuinely
// issued by that intermediate rather than two unrelated certificates concatenated,
// so the ordering assertion below is meaningful.

constexpr const char* ChainCertificates = R"(-----BEGIN CERTIFICATE-----
MIIB1zCCAXygAwIBAgIBAjAKBggqhkjOPQQDAjAmMSQwIgYDVQQDDBtOZXN0RHJp
dmUgVGVzdCBJbnRlcm1lZGlhdGUwIBcNMjYxMDIyMTkzOTEzWhgPMjEyNjA5Mjgx
OTM5MTNaMCMxITAfBgNVBAMMGG5lc3Rkcml2ZS5leGFtcGxlLnRzLm5ldDBZMBMG
ByqGSM49AgEGCCqGSM49AwEHA0IABN9IB1uJDYbppvXedCivuO43Z5Ka5uBd8o6/
dvDyEU8EBRPL2idwLGFsD4AaGJN8gue8BKAUkhgT1Bz3K93yTOKjgZswgZgwIwYD
VR0RBBwwGoIYbmVzdGRyaXZlLmV4YW1wbGUudHMubmV0MAwGA1UdEwEB/wQCMAAw
DgYDVR0PAQH/BAQDAgWgMBMGA1UdJQQMMAoGCCsGAQUFBwMBMB0GA1UdDgQWBBQJ
vPxwkMoYN+rWnR5dzN673OeLATAfBgNVHSMEGDAWgBSc0+EPNuVrPWsJg3yeo7ZR
ep3YtTAKBggqhkjOPQQDAgNJADBGAiEA+ymiE+xnritkOSrlqKzm/RjpYntkbMco
W3msX692/QwCIQCF4281Nqgnf2PrAatpTMvtsv106QcGI5MAvdL/wRiXoA==
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIBkjCCATigAwIBAgIUCGGRghvhvYHEs+/dnXrWvtkcb/UwCgYIKoZIzj0EAwIw
JjEkMCIGA1UEAwwbTmVzdERyaXZlIFRlc3QgSW50ZXJtZWRpYXRlMCAXDTI2MTAy
MjE5MzkxM1oYDzIxMjYwOTI4MTkzOTEzWjAmMSQwIgYDVQQDDBtOZXN0RHJpdmUg
VGVzdCBJbnRlcm1lZGlhdGUwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATX6/Xp
g8BOtNqWOHf2KgDwHWTB5jq/GnivhsBC2uktH+NaBlO30ZCKht41WQ+reK6WVXIa
G0JN6OOlH7M2NcIZo0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIB
BjAdBgNVHQ4EFgQUnNPhDzblaz1rCYN8nqO2UXqd2LUwCgYIKoZIzj0EAwIDSAAw
RQIgAfhmVURqPnrdJTOeoBqmZyR51LBXwlBrAtk1zVb3VHICIQDYdPpfh7bilgJV
J2UTuwC8sk/CZdpZ4HQsrfLZ0WMV6g==
-----END CERTIFICATE-----
)";

constexpr const char* ChainLeafKey = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEICkJN9y5TPDtgG2f6yC3RMeJlpPOSxwHFsZmTp94iI5RoAoGCCqGSM49
AwEHoUQDQgAE30gHW4kNhumm9d50KK+47jdnkprm4F3yjr928PIRTwQFE8vaJ3As
YWwPgBoYk3yC57wEoBSSGBPUHPcr3fJM4g==
-----END EC PRIVATE KEY-----
)";

/// Subject common name of the leaf in ChainCertificates.
constexpr const char* ChainLeafName = "nestdrive.example.ts.net";

/// Subject common name of the intermediate in ChainCertificates.
constexpr const char* ChainIssuerName = "NestDrive Test Intermediate";

} // namespace

class TestTlsConfig : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void loadsAnEcKey();
    void loadsAnEcKeyInPkcs8Form();
    void loadsAnRsaKey();
    void keepsTheWholeCertificateChain();
    void putsTheLeafFirstInTheChain();
    void loadsASingleCertificateAsAChainOfOne();
    void reportsAMissingCertificate();
    void reportsAMissingKey();
    void reportsAMalformedCertificate();
    void reportsAnUnreadableKeyWithoutNamingRsa();
    void setsServerSideTlsPolicy();
    void describesTheActiveBackend();

private:
    /** Writes @p contents to a file in the temporary directory. */
    QString write(const QString& name, const char* contents);

    std::unique_ptr<QTemporaryDir> m_tmp;
};

void TestTlsConfig::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
}

void TestTlsConfig::cleanup()
{
    m_tmp.reset();
}

QString TestTlsConfig::write(const QString& name, const char* contents)
{
    const QString path = m_tmp->path() + QLatin1Char('/') + name;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};

    file.write(contents);
    file.close();

    return path;
}

void TestTlsConfig::loadsAnEcKey()
{
    // The defect this suite exists for. The loader asked QSslKey for an RSA key and
    // nothing else, so an ECDSA P-256 key — which is what `tailscale cert` writes,
    // and what most current ACME clients default to — was reported as "not a valid
    // RSA PEM key". Entirely unreachable with the self-signed RSA pair that
    // scripts/gen-cert.sh produces, which is why it survived two releases.
    const QString certPath = write(QStringLiteral("ec.crt"), EcCertificate);
    const QString keyPath  = write(QStringLiteral("ec.key"), EcKeySec1);
    QVERIFY(!certPath.isEmpty() && !keyPath.isEmpty());

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY2(configuration.has_value(), qPrintable(error));
    QVERIFY(error.isEmpty());

    QCOMPARE(configuration->privateKey().algorithm(), QSsl::Ec);
    QVERIFY(!configuration->privateKey().isNull());
}

void TestTlsConfig::loadsAnEcKeyInPkcs8Form()
{
    // Same key, generic "BEGIN PRIVATE KEY" banner. Worth its own test because the
    // header gives no clue what is inside: both RSA and EC keys arrive under it, so
    // this is the case that proves the loader is genuinely probing rather than
    // reading the banner text.
    const QString certPath = write(QStringLiteral("ec.crt"), EcCertificate);
    const QString keyPath  = write(QStringLiteral("ec-pk8.key"), EcKeyPkcs8);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY2(configuration.has_value(), qPrintable(error));
    QCOMPARE(configuration->privateKey().algorithm(), QSsl::Ec);
}

void TestTlsConfig::loadsAnRsaKey()
{
    // The case that already worked, and the one every existing installation depends
    // on. Teaching the loader about EC must not cost it.
    const QString certPath = write(QStringLiteral("rsa.crt"), RsaCertificate);
    const QString keyPath  = write(QStringLiteral("rsa.key"), RsaKey);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY2(configuration.has_value(), qPrintable(error));
    QCOMPARE(configuration->privateKey().algorithm(), QSsl::Rsa);
}

void TestTlsConfig::keepsTheWholeCertificateChain()
{
    // The second defect. The loader read one certificate and called
    // setLocalCertificate(), so the intermediate in a real certificate file was
    // dropped on the floor. A browser that happens to cache the intermediate, or
    // that fetches it via the AIA extension, still works — which is why this would
    // have looked fine in Chrome on a desktop and failed on a phone.
    const QString certPath = write(QStringLiteral("fullchain.crt"), ChainCertificates);
    const QString keyPath  = write(QStringLiteral("leaf.key"), ChainLeafKey);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY2(configuration.has_value(), qPrintable(error));
    QCOMPARE(configuration->localCertificateChain().size(), 2);
}

void TestTlsConfig::putsTheLeafFirstInTheChain()
{
    // Order is load-bearing, not cosmetic: TLS requires the server's own certificate
    // first, with each following one vouching for the previous. Reversing them
    // produces a handshake that fails for a reason no message explains well.
    const QString certPath = write(QStringLiteral("fullchain.crt"), ChainCertificates);
    const QString keyPath  = write(QStringLiteral("leaf.key"), ChainLeafKey);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);
    QVERIFY2(configuration.has_value(), qPrintable(error));

    const QList<QSslCertificate> chain = configuration->localCertificateChain();
    QCOMPARE(chain.size(), 2);

    const auto commonName = [](const QSslCertificate& certificate) {
        return certificate.subjectInfo(QSslCertificate::CommonName).value(0);
    };

    QCOMPARE(commonName(chain.at(0)), QLatin1String(ChainLeafName));
    QCOMPARE(commonName(chain.at(1)), QLatin1String(ChainIssuerName));

    // And the second really does vouch for the first, so this is a chain rather than
    // two certificates that happen to share a file.
    QCOMPARE(chain.at(0).issuerInfo(QSslCertificate::CommonName).value(0),
             QLatin1String(ChainIssuerName));
}

void TestTlsConfig::loadsASingleCertificateAsAChainOfOne()
{
    // The self-signed LAN case, expressed through the new chain API. Nothing about
    // reading a chain may penalise the file that holds exactly one certificate.
    const QString certPath = write(QStringLiteral("ec.crt"), EcCertificate);
    const QString keyPath  = write(QStringLiteral("ec.key"), EcKeySec1);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY2(configuration.has_value(), qPrintable(error));
    QCOMPARE(configuration->localCertificateChain().size(), 1);
}

void TestTlsConfig::reportsAMissingCertificate()
{
    const QString keyPath = write(QStringLiteral("ec.key"), EcKeySec1);

    QString error;
    const auto configuration =
        core::loadTls(m_tmp->path() + QStringLiteral("/absent.crt"), keyPath, &error);

    QVERIFY(!configuration.has_value());

    // The message names the path. These reach the user directly in the window's
    // status label, where "could not load certificate" would leave them with nothing
    // to act on.
    QVERIFY2(error.contains(QStringLiteral("absent.crt")), qPrintable(error));
}

void TestTlsConfig::reportsAMissingKey()
{
    const QString certPath = write(QStringLiteral("ec.crt"), EcCertificate);

    QString error;
    const auto configuration =
        core::loadTls(certPath, m_tmp->path() + QStringLiteral("/absent.key"), &error);

    QVERIFY(!configuration.has_value());
    QVERIFY2(error.contains(QStringLiteral("absent.key")), qPrintable(error));
}

void TestTlsConfig::reportsAMalformedCertificate()
{
    const QString certPath =
        write(QStringLiteral("junk.crt"), "this is not a certificate\n");
    const QString keyPath = write(QStringLiteral("ec.key"), EcKeySec1);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY(!configuration.has_value());
    QVERIFY2(error.contains(QStringLiteral("not valid PEM")), qPrintable(error));
}

void TestTlsConfig::reportsAnUnreadableKeyWithoutNamingRsa()
{
    const QString certPath = write(QStringLiteral("ec.crt"), EcCertificate);
    const QString keyPath  = write(QStringLiteral("junk.key"), "not a key either\n");

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);

    QVERIFY(!configuration.has_value());

    // Specifically asserts the old wording is gone. "Private key is not a valid RSA
    // PEM key" was worse than unhelpful on an EC key: it blamed the file for this
    // code's refusal to consider any other algorithm, which is exactly the wrong
    // place to send somebody looking.
    QVERIFY2(!error.contains(QStringLiteral("RSA private key")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("junk.key")), qPrintable(error));
}

void TestTlsConfig::setsServerSideTlsPolicy()
{
    const QString certPath = write(QStringLiteral("ec.crt"), EcCertificate);
    const QString keyPath  = write(QStringLiteral("ec.key"), EcKeySec1);

    QString error;
    const auto configuration = core::loadTls(certPath, keyPath, &error);
    QVERIFY2(configuration.has_value(), qPrintable(error));

    // Carried over from the original loader and worth pinning. TLS 1.0 and 1.1 are
    // deprecated and a browser will refuse them anyway.
    QCOMPARE(configuration->protocol(), QSsl::TlsV1_2OrLater);

    // This server never asks clients for a certificate — authentication is the
    // session cookie's job — so verifying a peer that presented nothing would reject
    // every browser that connected.
    QCOMPARE(configuration->peerVerifyMode(), QSslSocket::VerifyNone);
}

void TestTlsConfig::describesTheActiveBackend()
{
    // Not an assertion about which backend, because that is a property of the
    // machine: Qt uses OpenSSL when it can load it and Schannel on Windows
    // otherwise. The point is that the string is populated, so the startup log
    // records what was actually in use rather than leaving it to be guessed at
    // halfway through debugging a handshake.
    const QString description = core::describeTlsBackend();

    QVERIFY(!description.isEmpty());

    if (QSslSocket::supportsSsl()) {
        QVERIFY2(description.contains(QSslSocket::activeBackend()),
                 qPrintable(description));
    }

    // Recorded in the test log too — this is the first time anything in the suite
    // has reported it, and the answer decides whether the Schannel path is the one
    // under test on this machine.
    qInfo() << "TLS backend:" << description;
}

QTEST_MAIN(TestTlsConfig)
#include "tst_tlsconfig.moc"
