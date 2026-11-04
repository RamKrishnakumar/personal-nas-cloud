#include "core/Tls/Tailscale.h"

#include "fixtures/NoTailscaleOnPath.h"

#include <QDir>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

/// What the resolver appends when looking for the binary on this platform.
#ifdef Q_OS_WIN
#  define TailscaleSuffix ".exe"
#else
#  define TailscaleSuffix ""
#endif

/**
 * @file
 * @brief Coverage for core::Tailscale, the CLI wrapper.
 *
 * Runs against tests/fixtures/fake_tailscale.cpp rather than the real tool, which
 * would need a daemon, a logged-in node, a tailnet with HTTPS enabled and a round
 * trip to Let's Encrypt. What this class has to get right is the plumbing — the
 * arguments it builds, the JSON it reads, and what it does when the tool misbehaves
 * — and all of that is reachable with a fixture that misbehaves on request.
 */
class TestTailscale : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void reportsUnavailableWithoutABinary();
    void fallsBackToPathWhenTheConfiguredPathIsWrong();
    void reportsUnavailableWhenTheVersionProbeFails();
    void readsTheVersionLine();

    void readsTheMagicDnsNameWithoutItsRootLabel();
    void readsTheOverlayAddress();
    void prefersIpv4WhenBothAreOffered();
    void reportsOfflineWhenLoggedOut();
    void survivesUnparseableStatus();

    void writesACertificateAndKey();
    void passesTheRequestedPathsToTheCli();
    void refusesToIssueWithoutADnsName();
    void surfacesTheHttpsNotEnabledMessage();
    void failsWhenTheCliWritesNothing();

private:
    /** A wrapper pointed at the fixture, with @p mode selected. */
    std::unique_ptr<core::Tailscale> fake(const QByteArray& mode = "ok");

    QString certPath() const { return m_tmp->path() + QStringLiteral("/out.crt"); }
    QString keyPath()  const { return m_tmp->path() + QStringLiteral("/out.key"); }

    std::unique_ptr<QTemporaryDir> m_tmp;
};

void TestTailscale::init()
{
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
}

void TestTailscale::cleanup()
{
    qunsetenv("FAKE_TAILSCALE_MODE");
    qunsetenv("FAKE_TAILSCALE_LOG");
    m_tmp.reset();
}

std::unique_ptr<core::Tailscale> TestTailscale::fake(const QByteArray& mode)
{
    // Set before construction: the availability probe runs in the constructor, so a
    // mode applied afterwards would arrive too late to affect it.
    qputenv("FAKE_TAILSCALE_MODE", mode);
    return std::make_unique<core::Tailscale>(QStringLiteral(FAKE_TAILSCALE_PATH));
}

void TestTailscale::reportsUnavailableWithoutABinary()
{
    // The ordinary case on a machine where Tailscale is not installed, and it must
    // stay ordinary: the server still serves the LAN, so this is a configuration
    // rather than a fault.
    //
    // The guard is what makes that question answerable here — see the long note in
    // fixtures/NoTailscaleOnPath.h. Without it this test says nothing about the code
    // and everything about whether the machine running it happens to have Tailscale.
    const NoTailscaleOnPath guard;

    const core::Tailscale tailscale(m_tmp->path() + QStringLiteral("/not-here.exe"));

    QVERIFY(!tailscale.isAvailable());
    QVERIFY(tailscale.versionLine().isEmpty());
}

void TestTailscale::fallsBackToPathWhenTheConfiguredPathIsWrong()
{
    // The other half of the contract above, and the reason the guard has to exist at
    // all: an unrunnable configured path does NOT mean give up. Here the fixture is
    // placed on PATH under the name the resolver looks for, so the fallback has
    // something real to find.
    QVERIFY(m_tmp->isValid());

    const QString binDirectory = m_tmp->path() + QStringLiteral("/bin");
    QVERIFY(QDir().mkpath(binDirectory));

    const QString onPath = binDirectory + QStringLiteral("/tailscale")
                           + QStringLiteral(TailscaleSuffix);
    QVERIFY2(QFile::copy(QStringLiteral(FAKE_TAILSCALE_PATH), onPath),
             "could not place the fixture on PATH");

    // Prepended rather than substituted. Emptying PATH — which is what the guard in
    // the test above does — would also hide the Qt DLLs the fixture itself links, so
    // the copy would be found and then fail to start, and the test would report a
    // broken fallback when the fallback was fine. Prepending shadows any real
    // installation while leaving the loader able to do its job.
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH",
            QDir::toNativeSeparators(binDirectory).toLocal8Bit()
                + QDir::listSeparator().toLatin1() + savedPath);

    qputenv("FAKE_TAILSCALE_MODE", QByteArray("ok"));
    const core::Tailscale tailscale(m_tmp->path() + QStringLiteral("/not-here.exe"));

    qputenv("PATH", savedPath);

    QVERIFY2(tailscale.isAvailable(),
             "a wrong configured path must fall back to PATH, not give up");
}

void TestTailscale::reportsUnavailableWhenTheVersionProbeFails()
{
    // A binary that exists but cannot run — wrong architecture, missing runtime.
    // Finding the file is not the same as being able to use it.
    const auto tailscale = fake("bad_version");
    QVERIFY(!tailscale->isAvailable());
}

void TestTailscale::readsTheVersionLine()
{
    const auto tailscale = fake();

    QVERIFY(tailscale->isAvailable());
    QCOMPARE(tailscale->versionLine(), QStringLiteral("1.78.1"));

    // The first line only. `tailscale version` prints commit details underneath, and
    // the startup log wants the version rather than a paragraph.
    QVERIFY(!tailscale->versionLine().contains(QLatin1Char('\n')));
}

void TestTailscale::readsTheMagicDnsNameWithoutItsRootLabel()
{
    // The API reports names fully qualified, with the root label:
    // "nestdrive.tail1a2b3c.ts.net." — and that trailing dot has to go. Left on, it
    // reaches both the URL shown in the window and the argument to `tailscale cert`.
    const auto tailscale = fake();
    const core::TailscaleStatus status = tailscale->status();

    QCOMPARE(status.dnsName, QStringLiteral("nestdrive.tail1a2b3c.ts.net"));
    QVERIFY(!status.dnsName.endsWith(QLatin1Char('.')));
    QVERIFY(status.isOnline);
}

void TestTailscale::readsTheOverlayAddress()
{
    const auto tailscale = fake();
    QCOMPARE(tailscale->status().address, QStringLiteral("100.101.102.103"));
}

void TestTailscale::prefersIpv4WhenBothAreOffered()
{
    // The overlay assigns both families and the order is not guaranteed. Picking the
    // IPv6 address by accident yields a URL that needs bracketing and that not every
    // client on the tailnet can route.
    const auto tailscale = fake("ipv6_first");
    const core::TailscaleStatus status = tailscale->status();

    QCOMPARE(status.address, QStringLiteral("100.101.102.103"));
    QVERIFY(!status.address.contains(QLatin1Char(':')));
}

void TestTailscale::reportsOfflineWhenLoggedOut()
{
    // Installed but not logged in. The CLI exits zero and answers normally, so this
    // is a successful call reporting an unusable node — not an error.
    const auto tailscale = fake("logged_out");
    const core::TailscaleStatus status = tailscale->status();

    QVERIFY(tailscale->isAvailable());
    QVERIFY(!status.isOnline);
    QVERIFY(status.dnsName.isEmpty());
    QVERIFY(status.address.isEmpty());
}

void TestTailscale::survivesUnparseableStatus()
{
    // A future version changing its output, or a wrapper script printing a warning
    // first. An empty status is the right answer; a crash is not.
    const auto tailscale = fake("bad_json");
    const core::TailscaleStatus status = tailscale->status();

    QVERIFY(!status.isOnline);
    QVERIFY(status.dnsName.isEmpty());
}

void TestTailscale::writesACertificateAndKey()
{
    const auto tailscale = fake();

    QString error;
    QVERIFY2(tailscale->issueCert(QStringLiteral("nestdrive.tail1a2b3c.ts.net"),
                                 certPath(), keyPath(), &error),
             qPrintable(error));

    // And what it wrote is the shape that broke the old loader: an EC key, and more
    // than one certificate in the file. Asserted here as well as in tst_tlsconfig so
    // the fixture cannot quietly drift into producing the easy case instead.
    const QList<QSslCertificate> chain =
        QSslCertificate::fromPath(certPath(), QSsl::Pem);
    QCOMPARE(chain.size(), 2);

    QFile key(keyPath());
    QVERIFY(key.open(QIODevice::ReadOnly));
    const QSslKey parsed(&key, QSsl::Ec, QSsl::Pem);
    QVERIFY2(!parsed.isNull(), "the fixture must write a usable EC key");
}

void TestTailscale::passesTheRequestedPathsToTheCli()
{
    const QString logPath = m_tmp->path() + QStringLiteral("/args.log");

    const auto tailscale = fake();
    qputenv("FAKE_TAILSCALE_LOG", logPath.toLocal8Bit());

    QString error;
    QVERIFY2(tailscale->issueCert(QStringLiteral("nestdrive.tail1a2b3c.ts.net"),
                                 certPath(), keyPath(), &error),
             qPrintable(error));

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString recorded = QString::fromUtf8(log.readAll());

    // Asserting on what was passed, not only on what came back. The name has to be
    // the last argument and the paths have to arrive under the right flags, neither
    // of which is visible from the return value.
    QVERIFY2(recorded.contains(QStringLiteral("--cert-file")), qPrintable(recorded));
    QVERIFY2(recorded.contains(QStringLiteral("--key-file")), qPrintable(recorded));
    QVERIFY2(recorded.contains(QStringLiteral("nestdrive.tail1a2b3c.ts.net")),
             qPrintable(recorded));
}

void TestTailscale::refusesToIssueWithoutADnsName()
{
    // Guarded here rather than left to the CLI. Without the name the tool would be
    // invoked with a flag and no argument, and its complaint would be about its own
    // usage rather than about the node being logged out.
    const auto tailscale = fake();

    QString error;
    QVERIFY(!tailscale->issueCert(QString(), certPath(), keyPath(), &error));
    QVERIFY2(error.contains(QStringLiteral("logged out")), qPrintable(error));
}

void TestTailscale::surfacesTheHttpsNotEnabledMessage()
{
    // By far the most common real failure, and the tool's own wording names both the
    // cause and the admin page to fix it on. Passing that through beats anything
    // this code could say instead, so the test pins that it is passed through.
    const auto tailscale = fake("no_https");

    QString error;
    QVERIFY(!tailscale->issueCert(QStringLiteral("nestdrive.tail1a2b3c.ts.net"),
                                 certPath(), keyPath(), &error));

    QVERIFY2(error.contains(QStringLiteral("HTTPS must be enabled")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("admin")), qPrintable(error));
}

void TestTailscale::failsWhenTheCliWritesNothing()
{
    // A zero exit with nothing on disk. Trusting the exit code alone would report
    // success and then fail in loadTls() a long way from the cause.
    const auto tailscale = fake("silent_success");

    QString error;
    QVERIFY(!tailscale->issueCert(QStringLiteral("nestdrive.tail1a2b3c.ts.net"),
                                 certPath(), keyPath(), &error));
    QVERIFY2(error.contains(QStringLiteral("did not write")), qPrintable(error));
}

QTEST_MAIN(TestTailscale)
#include "tst_tailscale.moc"
