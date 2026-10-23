#pragma once

#include <QString>
#include <QSettings>

/**
 * @class AppConfig
 * @brief Reads and provides access to all app configuration from config.ini.
 *
 * config.ini lives in the same folder as the executable and is NEVER committed
 * to GitHub. This keeps credentials safe while keeping the code open-source.
 *
 * To set up on a new machine:
 *   1. Copy config.ini.example → config.ini
 *   2. Fill in your real credentials in config.ini
 *   3. Run the app — it reads from config.ini automatically
 *
 * Uses Qt's QSettings with IniFormat — a simple key=value file format.
 */
class AppConfig {
public:
    /**
     * @brief Loads config.ini from the application's working directory.
     *
     * If config.ini doesn't exist, all getters return empty strings
     * and the app logs a warning. The app won't crash — DDNS simply
     * won't update without valid credentials.
     */
    explicit AppConfig();

    // ── DDNS Settings ──────────────────────────────────────────────
    QString ddnsUsername() const;  ///< No-IP account username
    QString ddnsPassword() const;  ///< No-IP account password
    QString ddnsHostname() const;  ///< DDNS hostname (e.g. nestdrive.ddns.org)

    // ── Server Settings ─────────────────────────────────────────────
    quint16 serverPort() const;    ///< HTTP server port (default: 9090)
    QString nasRoot()    const;    ///< Root directory to serve (default: E:/)

      // ── TLS (Step 1) ─────────────────────────────────────────────────────
    bool     useHttps()     const;   ///< true → start in HTTPS mode
    QString  certPath()     const;   ///< PEM certificate; leaf first, then any intermediates
    QString  keyPath()      const;   ///< PEM private key, EC or RSA, unencrypted

    // ── Remote access ────────────────────────────────────────────────────
    /**
     * @brief Whether to obtain and renew a certificate through Tailscale.
     *
     * Costs nothing when Tailscale is not installed or the node is logged out: the
     * server then keeps whatever certPath() points at, which for a LAN-only setup is
     * the self-signed pair from scripts/gen-cert.sh.
     *
     * Turn it off to manage the certificate by hand — a real public IP with
     * Let's Encrypt, for instance. See docs/REMOTE-ACCESS.md.
     */
    bool     useTailscaleCert() const;

    /**
     * @brief Path to the tailscale binary, or empty to search the usual places.
     *
     * Empty is the normal setting, with the same contract as ffmpegPath(): beside
     * the application first, then PATH.
     */
    QString  tailscalePath() const;

    /**
     * @brief How many days before expiry to renew. Default 14.
     *
     * Values at or below zero fall back to the default rather than being taken
     * literally — see core::CertRenewer::shouldRenew(), where zero would otherwise
     * mean "renew only after it has already expired".
     */
    int      certRenewDays() const;

    /**
     * @brief Which remote-access path to use: auto, tailscale, public or lan.
     *
     * Parsed by core::remoteMethodFromString(). Anything unrecognised reads as auto.
     */
    QString  remoteMethod() const;
    void     setRemoteMethod(const QString& value);

    /**
     * @brief A remembered answer about this connection, used only as a fallback.
     *
     * Detection runs at every startup rather than trusting this, because the answer
     * is a property of the connection and not of the machine — a laptop moved between
     * two providers, one behind carrier-grade NAT and one not, would otherwise be told
     * something true about the wrong network. This is consulted only when detection
     * comes back Unknown, which happens when the router will not report its address
     * over UPnP and the public address does not settle the question by itself.
     *
     * Parsed by core::connectionTypeFromString().
     */
    QString  connectionType() const;
    void     setConnectionType(const QString& value);

    /**
     * @brief Whether NestDrive may ask the router to forward its port.
     *
     * Off by default. Creating a rule on somebody's router is a change to equipment
     * they own, made on their behalf, and it should follow a deliberate press rather
     * than happen because the capability was detected.
     */
    bool     upnpPortForward() const;

    // ── Media (Step 17) ──────────────────────────────────────────────────
    /**
     * @brief Path to an ffmpeg binary, or empty to search the usual places.
     *
     * Empty is the normal setting: media::FrameExtractor then looks beside the
     * application and on PATH. Set it only to point at a specific build — a
     * configured path that does not exist is reported and then ignored.
     */
    QString  ffmpegPath()   const;

    // ── Meta ─────────────────────────────────────────────────────────────
    /**
     * @brief Returns true if config.ini was found and loaded successfully.
     * Use this to show a warning in the UI if credentials are missing.
     */
    bool isLoaded() const;

    void setDdnsUsername(const QString& value);
    void setDdnsPassword(const QString& value);
    void setDdnsHostname(const QString& value);
    void setServerPort(quint16 value);
    void setNasRoot(const QString& value);
    void setUseHttps(bool value);
    void setCertPath(const QString& value);
    void setKeyPath(const QString& value);
    bool save();

    static QString configPath();

private:
    /**
     * @brief Writes a starter config.ini with every key at its default value.
     *
     * Only called when no config.ini exists. QSettings cannot preserve comments,
     * so writing on every construction would strip the annotations out of a
     * config the user had hand-edited from config.ini.example.
     */
    void writeStarterConfig();

    /**
     * @brief Carries values over from key names used by earlier versions.
     *
     * Must run before any getter and before writeStarterConfig(), otherwise a
     * default would be written first and the legacy value never consulted.
     */
    void migrateLegacyKeys();

    QSettings m_settings; ///< Qt INI file reader
    bool      m_loaded;   ///< True if config.ini exists and was opened
};
