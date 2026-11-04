// A stand-in for the tailscale CLI, used by tst_tailscale and tst_certrenewer.
//
// The real tool needs a running daemon, a logged-in node, a tailnet with HTTPS
// enabled, and a round trip to Let's Encrypt. None of that belongs in a unit test,
// and none of it is what core::Tailscale has to get right. What it does have to get
// right is the plumbing: the arguments it builds, the JSON it parses, the trailing
// dot it strips, which address it picks out of several, and what it does when the
// tool fails or writes nothing. All of that is testable against a program that
// behaves badly on request.
//
// Behaviour is chosen by the FAKE_TAILSCALE_MODE environment variable:
//
//   ok              Report a logged-in node; write a real EC key and a two
//                   certificate chain for `cert`. The default.
//   logged_out      Report BackendState "NeedsLogin" with no Self addresses.
//   no_https        Fail `cert` with the message the real tool gives when HTTPS
//                   certificates are not enabled for the tailnet.
//   silent_success  Exit 0 from `cert` having written nothing at all.
//   bad_version     Fail the version probe, like a wrong-architecture binary.
//   ipv6_first      Report the IPv6 address before the IPv4 one.
//   bad_json        Answer `status --json` with something that is not JSON.
//
// Every invocation appends its arguments to FAKE_TAILSCALE_LOG when that is set,
// which is how the tests assert on what was passed rather than only on what came
// back.

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

namespace {

QByteArray mode()
{
    const QByteArray value = qgetenv("FAKE_TAILSCALE_MODE");
    return value.isEmpty() ? QByteArrayLiteral("ok") : value;
}

void logArguments(const QStringList& arguments)
{
    const QString logPath = QString::fromLocal8Bit(qgetenv("FAKE_TAILSCALE_LOG"));
    if (logPath.isEmpty())
        return;

    QFile log(logPath);
    if (!log.open(QIODevice::Append | QIODevice::Text))
        return;

    QTextStream(&log) << arguments.join(QLatin1Char(' ')) << '\n';
}

// The leaf carries the trailing dot in DNSName on purpose: the real API reports
// fully-qualified names with the root label, and stripping it is behaviour worth
// testing rather than assuming.
constexpr const char* StatusRunning = R"({
  "BackendState": "Running",
  "Self": {
    "DNSName": "nestdrive.tail1a2b3c.ts.net.",
    "TailscaleIPs": ["100.101.102.103", "fd7a:115c:a1e0::1234"],
    "Online": true
  }
})";

constexpr const char* StatusIpv6First = R"({
  "BackendState": "Running",
  "Self": {
    "DNSName": "nestdrive.tail1a2b3c.ts.net.",
    "TailscaleIPs": ["fd7a:115c:a1e0::1234", "100.101.102.103"],
    "Online": true
  }
})";

constexpr const char* StatusLoggedOut = R"({
  "BackendState": "NeedsLogin",
  "Self": {
    "DNSName": "",
    "TailscaleIPs": []
  }
})";

// A real EC P-256 key and a real two-certificate chain, so that a renewal driven
// through this fixture ends in core::loadTls() actually parsing something. Published
// test material, worthless by construction — see the note in tst_tlsconfig.cpp.
//
// The leaf's subjectAltName is deliberately the SAME name StatusRunning reports above.
// That is not tidiness: core::CertRenewer now renews when the certificate on disk does
// not cover the name it is serving, so a fixture that issued a certificate for some
// other host would make the renewer correctly decide to renew on every single check,
// and the test asserting that a fresh certificate is left alone would fail. The real
// tool issues for the name you ask about, and so must this.
constexpr const char* CertChain = R"(-----BEGIN CERTIFICATE-----
MIIB3DCCAYKgAwIBAgIBAzAKBggqhkjOPQQDAjAmMSQwIgYDVQQDDBtOZXN0RHJp
dmUgVGVzdCBJbnRlcm1lZGlhdGUwIBcNMjYxMDIzMTgwMzMzWhgPMjEyNjA5Mjkx
ODAzMzNaMCYxJDAiBgNVBAMMG25lc3Rkcml2ZS50YWlsMWEyYjNjLnRzLm5ldDBZ
MBMGByqGSM49AgEGCCqGSM49AwEHA0IABOIaPXQJIYZ2UUvIEVsHxBf0rj2jb4rL
lOSgk/hr6UJCZPyv4SgNM3eYAmEtpIO+ZRMpzAyihy7uzoXgT4feuaSjgZ4wgZsw
JgYDVR0RBB8wHYIbbmVzdGRyaXZlLnRhaWwxYTJiM2MudHMubmV0MAwGA1UdEwEB
/wQCMAAwDgYDVR0PAQH/BAQDAgWgMBMGA1UdJQQMMAoGCCsGAQUFBwMBMB0GA1Ud
DgQWBBRVyt5hr72hY8VCis1AxbI8PbhHmDAfBgNVHSMEGDAWgBSc0+EPNuVrPWsJ
g3yeo7ZRep3YtTAKBggqhkjOPQQDAgNIADBFAiBRe1lCq399GwA4CitjG0D1SJ0P
YWALD8xgxx8prqGOewIhAIubBNyzVUp/YDgj5baKwoIXWIZV9bMy0O7j8z0XUWGi
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

constexpr const char* CertKey = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIMsCyb+5nzp3tTUP/mA6AcvOCPT346N0ZavFsp8I7pJVoAoGCCqGSM49
AwEHoUQDQgAE4ho9dAkhhnZRS8gRWwfEF/SuPaNvisuU5KCT+GvpQkJk/K/hKA0z
d5gCYS2kg75lEynMDKKHLu7OheBPh965pA==
-----END EC PRIVATE KEY-----
)";

bool writeFile(const QString& path, const char* contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.write(contents);
    return true;
}

/** Value following @p flag in @p arguments, or empty when absent. */
QString valueOf(const QStringList& arguments, const QString& flag)
{
    const int index = arguments.indexOf(flag);
    if (index < 0 || index + 1 >= arguments.size())
        return {};

    return arguments.at(index + 1);
}

int handleVersion()
{
    if (mode() == QByteArrayLiteral("bad_version")) {
        std::fputs("fake tailscale: refusing to report a version\n", stderr);
        return 1;
    }

    std::fputs("1.78.1\n  tailscale commit: fake\n", stdout);
    return 0;
}

int handleStatus(const QStringList& arguments)
{
    if (!arguments.contains(QStringLiteral("--json"))) {
        std::fputs("fake tailscale: only --json is implemented\n", stderr);
        return 2;
    }

    const QByteArray selected = mode();

    if (selected == QByteArrayLiteral("bad_json")) {
        std::fputs("this is not json at all\n", stdout);
        return 0;
    }

    const char* body = selected == QByteArrayLiteral("logged_out") ? StatusLoggedOut
                     : selected == QByteArrayLiteral("ipv6_first") ? StatusIpv6First
                                                                   : StatusRunning;

    std::fputs(body, stdout);
    return 0;
}

int handleCert(const QStringList& arguments)
{
    const QByteArray selected = mode();

    if (selected == QByteArrayLiteral("no_https")) {
        // Close to what the real tool says, because core::Tailscale passes this
        // through verbatim and the test asserts the user would see something
        // actionable.
        std::fputs("HTTPS must be enabled in the tailnet admin panel: "
                   "https://login.tailscale.com/admin/dns\n",
                   stderr);
        return 1;
    }

    if (selected == QByteArrayLiteral("silent_success")) {
        // The real tool does not do this, but a wrapper script or a half-finished
        // write could. Exiting 0 with nothing on disk must not be reported as
        // success, or the failure surfaces much later in loadTls() instead.
        return 0;
    }

    const QString certPath = valueOf(arguments, QStringLiteral("--cert-file"));
    const QString keyPath  = valueOf(arguments, QStringLiteral("--key-file"));

    if (certPath.isEmpty() || keyPath.isEmpty()) {
        std::fputs("fake tailscale: --cert-file and --key-file are required\n", stderr);
        return 2;
    }

    if (!writeFile(certPath, CertChain) || !writeFile(keyPath, CertKey)) {
        std::fputs("fake tailscale: could not write the requested files\n", stderr);
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();

    logArguments(arguments);

    const QString subcommand = arguments.value(0);

    if (subcommand == QLatin1String("version"))
        return handleVersion();

    if (subcommand == QLatin1String("status"))
        return handleStatus(arguments);

    if (subcommand == QLatin1String("cert"))
        return handleCert(arguments);

    std::fputs("fake tailscale: unknown subcommand\n", stderr);
    return 2;
}
