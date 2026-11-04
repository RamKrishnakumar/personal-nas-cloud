#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace core {

/**
 * @brief What the local Tailscale node reports about itself.
 *
 * Empty when the CLI is missing, the daemon is not running, or the node is logged
 * out. All three are ordinary states — the server still works on the LAN — so they
 * are not errors, just an absent remote address.
 */
struct TailscaleStatus
{
    /**
     * MagicDNS name, e.g. "nestdrive.tail1234.ts.net", with the trailing dot the
     * API includes already stripped.
     *
     * This, and not the address, is what a browser must be pointed at: the
     * certificate is issued for the name, so reaching the server by IP produces
     * exactly the warning that having a real certificate exists to remove.
     */
    QString dnsName;

    /** The node's address within the overlay, from 100.64.0.0/10. */
    QString address;

    /** True when the node is up and carrying both of the above. */
    bool isOnline = false;
};

/**
 * @class Tailscale
 * @brief Thin wrapper over the `tailscale` command-line tool.
 *
 * ### Why the CLI rather than a library
 *
 * The same reasoning that chose a subprocess over linking FFmpeg in §5.7 applies
 * here, and more strongly. Tailscale's own client is Go; there is no C or C++ library
 * to link. The alternative is speaking to the local daemon's HTTP API over a named
 * pipe on Windows and a Unix socket elsewhere, which means reimplementing
 * authentication against an interface Tailscale does not document as stable. The CLI
 * *is* the documented interface, and `tailscale cert` in particular already handles
 * the certificate caching and renewal timing that we would otherwise have to guess
 * at.
 *
 * ### Why this is not a networking dependency
 *
 * Nothing in NestDrive speaks WireGuard, and nothing here touches a socket.
 * Tailscale provides the transport underneath the whole process, invisibly; this
 * class only asks it two questions — what am I called, and please give me a
 * certificate. Carrier-grade NAT makes a rendezvous unavoidable (see
 * docs/REMOTE-ACCESS.md), and adopting one that already ships clients for Windows,
 * macOS, iOS and Android costs no transport code at all.
 *
 * Const and stateless after construction; QProcess is created per call.
 */
class Tailscale
{
public:
    /**
     * @brief Longest a CLI call may run before the process is killed.
     *
     * Generous because issuing a certificate is not local work: the daemon talks to
     * Let's Encrypt and waits for the DNS-01 challenge to be seen. The status query
     * is local and answers in milliseconds, so this ceiling only ever bites on the
     * certificate path or on a wedged daemon.
     */
    static constexpr int DefaultTimeoutMs = 90'000;

    /**
     * @param executableOverride Configured path, tried first. May be empty.
     */
    explicit Tailscale(const QString& executableOverride = QString());

    /** True when a usable binary was found and answered a version query. */
    bool isAvailable() const { return m_available; }

    /** Absolute path of the binary in use, or empty when none was found. */
    QString executable() const { return m_executable; }

    /** First line of `tailscale version`, for the startup log. */
    QString versionLine() const { return m_versionLine; }

    void setTimeoutMs(int milliseconds);
    int  timeoutMs() const { return m_timeoutMs; }

    /**
     * @brief Asks the local daemon what this node is called and whether it is up.
     *
     * Runs `tailscale status --json` and reads `Self`. A node that is logged out
     * answers successfully with `BackendState` not equal to "Running", which is
     * reported as an offline status rather than a failure.
     */
    TailscaleStatus status() const;

    /**
     * @brief Obtains a certificate for @p dnsName and writes it to disk.
     *
     * Runs `tailscale cert`, which returns the cached certificate until it is close
     * to expiry and only then renews — so calling this on a schedule is the intended
     * usage rather than something to avoid.
     *
     * The certificate file holds the full chain and the key is ECDSA P-256. Both are
     * why core::loadTls() had to stop assuming one RSA certificate per file.
     *
     * @param error Receives the CLI's own stderr on failure. Worth surfacing
     *        verbatim: by far the most common cause is HTTPS certificates not being
     *        enabled for the tailnet, and the tool says so in as many words along
     *        with the admin page to go and fix it on.
     *
     * @return true when both files were written.
     */
    bool issueCert(const QString& dnsName,
                   const QString& certificatePath,
                   const QString& privateKeyPath,
                   QString*       error = nullptr) const;

private:
    /** Searches the configured path, the application directory, then PATH. */
    static QString resolveExecutable(const QString& executableOverride);

    /**
     * @brief Runs the CLI and captures stdout.
     *
     * @param standardError Receives stderr, trimmed, whatever the exit code.
     * @return true on a clean exit with status zero.
     */
    bool run(const QStringList& arguments,
             QByteArray*        standardOutput,
             QString*           standardError) const;

    QString m_executable;
    QString m_versionLine;
    bool    m_available = false;
    int     m_timeoutMs = DefaultTimeoutMs;
};

} // namespace core
