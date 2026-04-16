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
    bool save();

    static QString configPath();

private:
    void ensureDefaults();
    QSettings m_settings; ///< Qt INI file reader
    bool      m_loaded;   ///< True if config.ini exists and was opened
};
