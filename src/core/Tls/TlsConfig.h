#pragma once

#include <QSslConfiguration>
#include <QString>

#include <optional>

namespace core {

/**
 * @brief Builds a server QSslConfiguration from a PEM certificate and private key.
 *
 * Lives here rather than in FileServer for two reasons. FileServer is documented as
 * a facade over the HTTP stack, and parsing PEM is not facade work; and FileServer
 * compiles straight into the application executable, so for the whole of v0.5 and
 * v0.6 no test could reach this code. Three defects accumulated in it unnoticed
 * because of that, all of which only bite on a certificate issued by a real CA:
 *
 *   - The key algorithm was hardcoded to RSA. `tailscale cert` and most modern ACME
 *     clients issue ECDSA P-256, which was rejected outright as "not a valid RSA
 *     PEM key".
 *   - Only the first certificate in the file was read. Real certificates arrive as
 *     a chain — leaf plus one or more intermediates — and serving the leaf alone
 *     fails validation on any client that does not chase AIA to fetch the missing
 *     link, which includes Safari and most Android apps.
 *
 * The self-signed LAN path never showed either problem, because scripts/gen-cert.sh
 * emits exactly one RSA certificate.
 *
 * @param certificatePath PEM file holding the leaf first, then any intermediates.
 *        A relative path resolves against the executable's directory, not the
 *        working directory.
 * @param privateKeyPath PEM private key, in SEC1 or PKCS#8 form, unencrypted. Qt
 *        cannot prompt for a passphrase at startup, so an encrypted key would stop
 *        the server from starting unattended.
 * @param error Set to a human-readable reason when loading fails. The messages reach
 *        the user directly in the desktop window's status label, so they name the
 *        offending path.
 *
 * @return The configuration, or nullopt with @p error set.
 */
std::optional<QSslConfiguration> loadTls(const QString& certificatePath,
                                         const QString& privateKeyPath,
                                         QString*       error);

/**
 * @brief Describes which TLS library Qt actually loaded.
 *
 * Qt picks a backend at runtime — OpenSSL when its shared libraries can be found,
 * otherwise Schannel on Windows, otherwise a certificate-only stub that cannot
 * encrypt anything at all. Which one it lands on depends on what happens to be on
 * PATH, so it is a deployment property, not a build property, and worth recording
 * in the log before anyone tries to debug a handshake.
 */
QString describeTlsBackend();

/**
 * @brief Resolves a configured certificate or key path.
 *
 * Relative paths resolve against the executable's directory, never the working
 * directory — config.ini's defaults are the bare names `cert.pem` and `key.pem`, and
 * CMake copies those next to the binary.
 *
 * Exposed because three places need to agree on it: loadTls(), CertRenewer, and the
 * window's check on a certificate it does not manage. Two of them resolving
 * differently would have the UI reporting on a different file from the one being
 * served — which is a confusing thing to debug and an easy thing to prevent.
 */
QString resolveTlsPath(const QString& path);

} // namespace core
