#include <QTest>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include "http/PathResolver.h"

using http::resolveSafe;

/**
 * Tests for http::resolveSafe.
 *
 * A defect here is a file-disclosure bug, so the escape cases matter more than
 * the happy path. Two specific v0.5 defects are covered:
 *
 *   1. `fsPath.startsWith(nasRoot)` accepted a sibling directory, so a root of
 *      `E:/Foo` leaked `E:/Foobar`.
 *   2. The comparison was case-sensitive on Windows, where the filesystem is
 *      not, so legitimate requests were refused.
 */
class TestPathResolver : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void allowed_data();
    void allowed();
    void refused_data();
    void refused();

    void siblingDirectoryIsRefused();
    void rootItselfResolves();
    void missingFileStillResolves();
    void percentEncodedNamesDecode();
    void encodedTraversalIsRefused();
    void caseInsensitiveOnWindowsAndMac();
    void symlinkEscapeIsRefused();
    void linkInsideRootIsAllowed();
    void missingRootIsRefused();

private:
    QTemporaryDir m_tmp;
    QString       m_root;      ///< <tmp>/nasroot  — the served directory
    QString       m_outside;   ///< <tmp>/outside  — must never be reachable
    QString       m_sibling;   ///< <tmp>/nasrootX — prefix-collision probe
};

// ─────────────────────────────────────────────────────────────────────────────

void TestPathResolver::initTestCase()
{
    QVERIFY2(m_tmp.isValid(), qPrintable(m_tmp.errorString()));

    const QDir base(m_tmp.path());
    m_root    = base.absoluteFilePath(QStringLiteral("nasroot"));
    m_outside = base.absoluteFilePath(QStringLiteral("outside"));
    m_sibling = base.absoluteFilePath(QStringLiteral("nasrootX"));

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Photos/2024")));
    QVERIFY(QDir().mkpath(m_outside));
    QVERIFY(QDir().mkpath(m_sibling));

    // A real file inside the root, and a secret outside it.
    QFile inside(m_root + QStringLiteral("/Photos/2024/IMG 0421.jpg"));
    QVERIFY(inside.open(QIODevice::WriteOnly));
    inside.write("jpeg");
    inside.close();

    QFile secret(m_outside + QStringLiteral("/secret.txt"));
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("do not serve this");
    secret.close();

    QFile leak(m_sibling + QStringLiteral("/leak.txt"));
    QVERIFY(leak.open(QIODevice::WriteOnly));
    leak.write("sibling directory");
    leak.close();
}

void TestPathResolver::allowed_data()
{
    QTest::addColumn<QString>("urlPath");

    QTest::newRow("root")            << QStringLiteral("/");
    QTest::newRow("empty")           << QStringLiteral("");
    QTest::newRow("directory")       << QStringLiteral("/Photos");
    QTest::newRow("nested dir")      << QStringLiteral("/Photos/2024");
    QTest::newRow("trailing slash")  << QStringLiteral("/Photos/2024/");
    QTest::newRow("double slash")    << QStringLiteral("//Photos//2024");
    QTest::newRow("dot segment")     << QStringLiteral("/Photos/./2024");
}

void TestPathResolver::allowed()
{
    QFETCH(QString, urlPath);

    const auto resolved = resolveSafe(m_root, urlPath);
    QVERIFY2(resolved.has_value(), qPrintable(urlPath));
    QVERIFY2(resolved->startsWith(QDir(m_root).canonicalPath()),
             qPrintable(*resolved));
}

void TestPathResolver::refused_data()
{
    QTest::addColumn<QString>("urlPath");

    QTest::newRow("parent")               << QStringLiteral("/..");
    QTest::newRow("parent file")          << QStringLiteral("/../outside/secret.txt");
    QTest::newRow("deep traversal")       << QStringLiteral("/../../../../etc/passwd");
    QTest::newRow("traversal mid path")   << QStringLiteral("/Photos/../../outside/secret.txt");
    QTest::newRow("backslash traversal")  << QStringLiteral("/..\\outside\\secret.txt");
    QTest::newRow("mixed separators")     << QStringLiteral("/Photos\\..\\..\\outside");
    QTest::newRow("nul byte")             << QStringLiteral("/Photos/a%00b.jpg");
    QTest::newRow("newline")              << QStringLiteral("/Photos/a%0Ab.jpg");

#if defined(Q_OS_WIN)
    QTest::newRow("alternate data stream") << QStringLiteral("/Photos/file.txt:hidden");
    QTest::newRow("device CON")            << QStringLiteral("/CON");
    QTest::newRow("device CON.txt")        << QStringLiteral("/CON.txt");
    QTest::newRow("device NUL")            << QStringLiteral("/Photos/NUL");
    QTest::newRow("device COM1")           << QStringLiteral("/COM1");
    QTest::newRow("trailing dot")          << QStringLiteral("/Photos/secret.txt.");
    QTest::newRow("trailing space")        << QStringLiteral("/Photos/secret.txt%20");
    QTest::newRow("drive absolute")        << QStringLiteral("/C:/Windows/System32");
#endif
}

void TestPathResolver::refused()
{
    QFETCH(QString, urlPath);

    const auto resolved = resolveSafe(m_root, urlPath);
    QVERIFY2(!resolved.has_value(),
             qPrintable(QStringLiteral("%1 resolved to %2")
                            .arg(urlPath, resolved.value_or(QString()))));
}

void TestPathResolver::siblingDirectoryIsRefused()
{
    // The v0.5 bug: startsWith("<tmp>/nasroot") accepts "<tmp>/nasrootX".
    // Reaching the sibling requires traversal, which must be refused, and the
    // sibling's own path must never be produced.
    const auto resolved = resolveSafe(m_root, QStringLiteral("/../nasrootX/leak.txt"));
    QVERIFY(!resolved.has_value());

    // Guard the prefix logic directly too: a root of ".../nasroot" must not
    // consider ".../nasrootX" to be inside it.
    const auto fromSibling = resolveSafe(m_sibling, QStringLiteral("/leak.txt"));
    QVERIFY(fromSibling.has_value());
    QVERIFY2(!fromSibling->startsWith(QDir(m_root).canonicalPath() + QLatin1Char('/')),
             qPrintable(*fromSibling));
}

void TestPathResolver::rootItselfResolves()
{
    const auto resolved = resolveSafe(m_root, QStringLiteral("/"));
    QVERIFY(resolved.has_value());
    QCOMPARE(QDir(*resolved).canonicalPath(), QDir(m_root).canonicalPath());
}

void TestPathResolver::missingFileStillResolves()
{
    // A legitimate path to a file that does not exist must resolve, so the
    // caller can answer 404 rather than 403. Conflating the two tells an
    // attacker which paths exist.
    const auto resolved = resolveSafe(m_root, QStringLiteral("/Photos/nope.jpg"));
    QVERIFY(resolved.has_value());
    QVERIFY(!QFile::exists(*resolved));
}

void TestPathResolver::percentEncodedNamesDecode()
{
    const auto resolved =
        resolveSafe(m_root, QStringLiteral("/Photos/2024/IMG%200421.jpg"));
    QVERIFY(resolved.has_value());
    QVERIFY2(QFile::exists(*resolved), qPrintable(*resolved));
}

void TestPathResolver::encodedTraversalIsRefused()
{
    // After a single decode these are literal ".." segments, which are refused.
    for (const QString& urlPath : { QStringLiteral("/%2e%2e/outside/secret.txt"),
                                    QStringLiteral("/%2E%2E/outside/secret.txt"),
                                    QStringLiteral("/Photos/%2e%2e/%2e%2e/outside"),
                                    QStringLiteral("/..%2foutside%2fsecret.txt") }) {
        const auto resolved = resolveSafe(m_root, urlPath);
        QVERIFY2(!resolved.has_value(), qPrintable(urlPath));
    }
}

void TestPathResolver::caseInsensitiveOnWindowsAndMac()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // The filesystem is case-insensitive here, so a case-varied request is
    // legitimate and must not be refused.
    const auto resolved = resolveSafe(m_root, QStringLiteral("/photos/2024"));
    QVERIFY2(resolved.has_value(), "Case-varied path refused on a case-insensitive filesystem");
#else
    QSKIP("Filesystem is case-sensitive on this platform");
#endif
}

void TestPathResolver::symlinkEscapeIsRefused()
{
    const QString linkPath = m_root + QStringLiteral("/escape");

    // QFile::link() is not usable here on Windows: it writes a .lnk shortcut,
    // which is an ordinary file the filesystem does not follow. A shortcut is
    // therefore not an escape at all, and a test built on one would pass while
    // verifying nothing. Create a real link per platform instead.
#if defined(Q_OS_WIN)
    // A directory junction is the right target: unlike a symlink it needs no
    // Developer Mode or elevation, and it is the escape an attacker would
    // actually plant inside a served folder.
    const int rc = QProcess::execute(QStringLiteral("cmd"), {
        QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
        QDir::toNativeSeparators(linkPath),
        QDir::toNativeSeparators(m_outside)
    });
    if (rc != 0)
        QSKIP("Could not create a directory junction on this account");
#else
    if (!QFile::link(m_outside, linkPath))
        QSKIP("Could not create a symlink on this platform/account");
#endif

    QVERIFY2(QFileInfo::exists(linkPath + QStringLiteral("/secret.txt")),
             "Test setup is wrong: the link does not expose the outside file, "
             "so this test would pass without proving anything");

    // String normalisation cannot see this; only the canonical-path re-check can.
    const auto resolved = resolveSafe(m_root, QStringLiteral("/escape/secret.txt"));

    // Captured before the link is torn down, so the failure message below can
    // report what Qt actually saw.
    const QFileInfo linkInfo(linkPath);
    const QString diagIsSymLink   = linkInfo.isSymLink() ? QStringLiteral("true")
                                                         : QStringLiteral("false");
    const QString diagLinkTarget  = linkInfo.symLinkTarget();
    const QString diagRootCanon   = QFileInfo(m_root).canonicalFilePath();
    const QString diagFileCanon   =
        QFileInfo(linkPath + QStringLiteral("/secret.txt")).canonicalFilePath();

#if defined(Q_OS_WIN)
    QProcess::execute(QStringLiteral("cmd"), {
        QStringLiteral("/c"), QStringLiteral("rmdir"),
        QDir::toNativeSeparators(linkPath)
    });
#else
    QFile::remove(linkPath);
#endif

    QVERIFY2(!resolved.has_value(),
             qPrintable(QStringLiteral(
                 "A link pointing outside the root must be refused.\n"
                 "  resolved to      : %1\n"
                 "  root canonical   : %2\n"
                 "  file canonical   : %3\n"
                 "  Qt isSymLink     : %4\n"
                 "  Qt symLinkTarget : %5")
                 .arg(resolved.value_or(QStringLiteral("<refused>")),
                      diagRootCanon,
                      diagFileCanon,
                      diagIsSymLink,
                      diagLinkTarget)));
}

void TestPathResolver::linkInsideRootIsAllowed()
{
    // The escape check must not become an outright ban on links. Junctioning one
    // folder to another on the same drive is a normal way to organise storage on
    // Windows, and refusing it would 403 a legitimate folder.
    const QString linkPath   = m_root + QStringLiteral("/album-link");
    const QString linkTarget = m_root + QStringLiteral("/Photos/2024");

#if defined(Q_OS_WIN)
    const int rc = QProcess::execute(QStringLiteral("cmd"), {
        QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
        QDir::toNativeSeparators(linkPath),
        QDir::toNativeSeparators(linkTarget)
    });
    if (rc != 0)
        QSKIP("Could not create a directory junction on this account");
#else
    if (!QFile::link(linkTarget, linkPath))
        QSKIP("Could not create a symlink on this platform/account");
#endif

    const auto resolved =
        resolveSafe(m_root, QStringLiteral("/album-link/IMG%200421.jpg"));

#if defined(Q_OS_WIN)
    QProcess::execute(QStringLiteral("cmd"), {
        QStringLiteral("/c"), QStringLiteral("rmdir"),
        QDir::toNativeSeparators(linkPath)
    });
#else
    QFile::remove(linkPath);
#endif

    QVERIFY2(resolved.has_value(),
             "A link whose target is inside the root must be allowed");
}

void TestPathResolver::missingRootIsRefused()
{
    // A misconfigured nasRoot must fail closed, not resolve to something else.
    const auto resolved = resolveSafe(m_tmp.path() + QStringLiteral("/does-not-exist"),
                                      QStringLiteral("/anything"));
    QVERIFY(!resolved.has_value());
}

QTEST_APPLESS_MAIN(TestPathResolver)
#include "tst_pathresolver.moc"
