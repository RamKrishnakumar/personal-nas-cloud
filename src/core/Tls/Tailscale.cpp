#include "core/Tls/Tailscale.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QStandardPaths>

namespace core {
namespace {

/// Both names, because Windows needs the suffix and the others must not have it.
constexpr const char* ExecutableNames[] = { "tailscale", "tailscale.exe" };

/**
 * @brief Strips the trailing dot from a DNS name.
 *
 * The status API reports fully-qualified names in their canonical form, with the
 * root label included: "nestdrive.tail1234.ts.net." — and that trailing dot has to
 * go before the name is handed to `tailscale cert` or put in a URL.
 */
QString withoutRootLabel(QString name)
{
    while (name.endsWith(QLatin1Char('.')))
        name.chop(1);
    return name;
}

} // namespace

Tailscale::Tailscale(const QString& executableOverride)
    : m_executable(resolveExecutable(executableOverride))
{
    if (m_executable.isEmpty())
        return;

    // Probing costs one process, done once here rather than per call. `version` is
    // used rather than `status` deliberately: it answers without the daemon running,
    // so a machine where Tailscale is installed but stopped still reports the binary
    // as available and gets a useful message from status() instead of looking as
    // though nothing is installed at all.
    QByteArray output;
    if (!run({ QStringLiteral("version") }, &output, nullptr))
        return;

    m_versionLine = QString::fromUtf8(output).split(QLatin1Char('\n')).value(0).trimmed();
    m_available   = !m_versionLine.isEmpty();
}

void Tailscale::setTimeoutMs(int milliseconds)
{
    if (milliseconds > 0)
        m_timeoutMs = milliseconds;
}

QString Tailscale::resolveExecutable(const QString& executableOverride)
{
    // 1. Configured explicitly, so a deliberate choice always wins.
    if (!executableOverride.isEmpty()) {
        const QFileInfo configured(executableOverride);
        if (configured.isFile() && configured.isExecutable())
            return configured.absoluteFilePath();

        qWarning() << "[Tailscale] Configured path is not a runnable file:"
                   << executableOverride << "— falling back to the usual places.";
    }

    // 2. Beside the application.
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    if (!applicationDirectory.isEmpty()) {
        for (const char* name : ExecutableNames) {
            const QFileInfo candidate(applicationDirectory + QLatin1Char('/')
                                      + QLatin1String(name));
            if (candidate.isFile() && candidate.isExecutable())
                return candidate.absoluteFilePath();
        }
    }

    // 3. On PATH, which is where a normal Tailscale install puts it.
    for (const char* name : ExecutableNames) {
        const QString found = QStandardPaths::findExecutable(QLatin1String(name));
        if (!found.isEmpty())
            return found;
    }

    return {};
}

bool Tailscale::run(const QStringList& arguments,
                    QByteArray*        standardOutput,
                    QString*           standardError) const
{
    if (m_executable.isEmpty())
        return false;

    QProcess process;
    process.start(m_executable, arguments);

    if (!process.waitForStarted(m_timeoutMs)) {
        if (standardError)
            *standardError = process.errorString();
        return false;
    }

    if (!process.waitForFinished(m_timeoutMs)) {
        // kill() rather than terminate(), for the reason recorded in §5.7 of the
        // plan: terminate() asks politely and a wedged process can ignore it, which
        // would park this thread until the application closed.
        process.kill();
        process.waitForFinished(1'000);

        if (standardError)
            *standardError = QStringLiteral("timed out after %1 ms").arg(m_timeoutMs);
        return false;
    }

    if (standardOutput)
        *standardOutput = process.readAllStandardOutput();

    if (standardError)
        *standardError = QString::fromUtf8(process.readAllStandardError()).trimmed();

    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

TailscaleStatus Tailscale::status() const
{
    TailscaleStatus result;

    QByteArray output;
    if (!run({ QStringLiteral("status"), QStringLiteral("--json") }, &output, nullptr))
        return result;

    const QJsonDocument document = QJsonDocument::fromJson(output);
    if (!document.isObject())
        return result;

    const QJsonObject root = document.object();

    // Read Self regardless of BackendState. A node that has just been brought up,
    // or one that is reachable but not yet fully "Running", still knows its own
    // name — and reporting the name while flagging it offline is more useful to the
    // window than reporting nothing at all.
    const QJsonObject self = root.value(QStringLiteral("Self")).toObject();

    result.dnsName = withoutRootLabel(self.value(QStringLiteral("DNSName")).toString());

    const QJsonArray addresses = self.value(QStringLiteral("TailscaleIPs")).toArray();
    for (const QJsonValue& value : addresses) {
        const QString address = value.toString();

        // IPv4 only. The overlay hands out an IPv6 address as well, and picking it
        // by accident would produce a URL that needs bracketing and that some
        // clients on the tailnet cannot route.
        if (!address.contains(QLatin1Char(':'))) {
            result.address = address;
            break;
        }
    }

    result.isOnline = root.value(QStringLiteral("BackendState")).toString()
                          == QLatin1String("Running")
                      && !result.dnsName.isEmpty();

    return result;
}

bool Tailscale::issueCert(const QString& dnsName,
                          const QString& certificatePath,
                          const QString& privateKeyPath,
                          QString*       error) const
{
    if (dnsName.isEmpty()) {
        if (error)
            *error = QStringLiteral("No MagicDNS name: the node is logged out.");
        return false;
    }

    QString stderrText;
    const bool ok = run({ QStringLiteral("cert"),
                          QStringLiteral("--cert-file"), certificatePath,
                          QStringLiteral("--key-file"), privateKeyPath,
                          dnsName },
                        nullptr,
                        &stderrText);

    if (!ok) {
        if (error) {
            // Verbatim. The overwhelmingly common failure is HTTPS certificates not
            // being enabled for the tailnet, and the CLI names both the problem and
            // the admin page to fix it on — which is far more use than anything this
            // code could say instead.
            *error = stderrText.isEmpty()
                         ? QStringLiteral("tailscale cert failed with no message.")
                         : stderrText;
        }
        return false;
    }

    // Trust the exit code only as far as checking the files arrived. A zero exit with
    // nothing written would otherwise be reported as success and then fail later in
    // loadTls(), a long way from the cause.
    for (const QString& path : { certificatePath, privateKeyPath }) {
        const QFileInfo written(path);
        if (!written.isFile() || written.size() == 0) {
            if (error)
                *error = QStringLiteral("tailscale cert reported success but did not "
                                        "write %1").arg(path);
            return false;
        }
    }

    return true;
}

} // namespace core
