#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>

/**
 * @brief Returns the path to the configuration file.
 * @return The path to config.ini.
 */
QString AppConfig::configPath()
{
    const QString appPath = QCoreApplication::applicationDirPath() + "/config.ini";
    if (QFile::exists(appPath)) return appPath;

    const QString cwdPath = QDir::currentPath() + "/config.ini";
    if (QFile::exists(cwdPath)) return cwdPath;

    return appPath; // default write location even if it doesn't exist yet
}

/**
 * @brief Looks for config.ini next to the running executable.
 *
 * QCoreApplication::applicationDirPath() returns the folder containing
 * MyNAS.exe — so config.ini should be placed there.
 *
 * During development in Qt Creator, the exe is in the build folder,
 * so place config.ini there. After deployment, place it next to the exe.
 */
AppConfig::AppConfig()
    : m_settings(configPath(), QSettings::IniFormat)
    , m_loaded(QFile::exists(configPath()))
{
    migrateLegacyKeys();

    if (!m_loaded) {
        qDebug() << "[Config] ⚠️ config.ini not found at" << configPath()
                 << "— writing one with default values.";
        writeStarterConfig();
    }
}

/**
 * @brief Moves values from key names used by earlier versions to current ones.
 *
 * v0.5's config.ini.example documented a [TLS] section:
 *
 *     [TLS]
 *     certificatePath=certs/server.crt
 *     privateKeyPath=certs/server.key
 *
 * but the code has always read Server/certPath and Server/keyPath. Anyone who
 * followed the example therefore had their cert paths silently ignored and fell
 * back to cert.pem / key.pem next to the executable.
 *
 * Carry the old values over so an existing config keeps working. A value already
 * present under the current key always wins. The legacy keys are left in place
 * rather than deleted — silently editing a user's config is worse than leaving a
 * stale key behind — but they are logged as ignored.
 */
void AppConfig::migrateLegacyKeys()
{
    struct Move { const char* legacy; const char* current; };
    static constexpr Move moves[] = {
        { "TLS/certificatePath", "Server/certPath" },
        { "TLS/privateKeyPath",  "Server/keyPath"  },
    };

    bool migrated = false;
    for (const Move& move : moves) {
        const QVariant legacyValue = m_settings.value(QLatin1String(move.legacy));
        if (legacyValue.isNull())
            continue;

        if (!m_settings.value(QLatin1String(move.current)).isNull()) {
            qDebug() << "[Config] ℹ️" << move.legacy << "ignored —"
                     << move.current << "is already set.";
            continue;
        }

        m_settings.setValue(QLatin1String(move.current), legacyValue);
        migrated = true;
        qDebug() << "[Config] ✅ Migrated" << move.legacy << "→" << move.current
                 << "=" << legacyValue.toString();
    }

    if (migrated)
        m_settings.sync();
}

/**
 * @brief Writes every key at its default value, creating config.ini.
 *
 * Only runs on first launch, when no config.ini exists. The getters below each
 * carry their own default, so this is not needed for reading — its purpose is
 * to leave the user a complete, editable file rather than an empty one.
 */
void AppConfig::writeStarterConfig()
{
    m_settings.setValue("Server/port",     9090);
    m_settings.setValue("Server/nasRoot",  "E:/");
    m_settings.setValue("Server/useHttps", false);
    m_settings.setValue("Server/certPath", "cert.pem");
    m_settings.setValue("Server/keyPath",  "key.pem");

    // Empty on purpose. The empty value means "look beside the application, then on
    // PATH", which is what should happen by default — writing a guess here would
    // instead produce a warning about a path that does not exist on most machines.
    m_settings.setValue("Media/ffmpegPath", "");

    m_settings.setValue("DDNS/username",   "");
    m_settings.setValue("DDNS/password",   "");
    m_settings.setValue("DDNS/hostname",   "");
    m_settings.sync();

    if (m_settings.status() != QSettings::NoError)
        qDebug() << "[Config] ❌ Could not write" << configPath()
                 << "— status:" << m_settings.status();
}

// ── DDNS Getters ──────────────────────────────────────────────────────────────

QString AppConfig::ddnsUsername() const
{
    // "DDNS/username" reads the 'username' key under the [DDNS] section
    // The second argument is the default value if key is missing
    return m_settings.value("DDNS/username", "").toString();
}

QString AppConfig::ddnsPassword() const
{
    return m_settings.value("DDNS/password", "").toString();
}

QString AppConfig::ddnsHostname() const
{
    return m_settings.value("DDNS/hostname", "").toString();
}

// ── Server Getters ────────────────────────────────────────────────────────────

quint16 AppConfig::serverPort() const
{
    // Returns 9090 as default if 'port' key is not found in config.ini
    return static_cast<quint16>(m_settings.value("Server/port", 9090).toInt());
}

QString AppConfig::nasRoot() const
{
    // Returns E:/ as default if 'nasRoot' key is not found in config.ini
    return m_settings.value("Server/nasRoot", "E:/").toString();
}


// ── TLS ───────────────────────────────────────────────────────────────────────
bool    AppConfig::useHttps()  const { return m_settings.value("Server/useHttps", false).toBool(); }
QString AppConfig::certPath()  const { return m_settings.value("Server/certPath", "cert.pem").toString(); }
QString AppConfig::keyPath()   const { return m_settings.value("Server/keyPath",  "key.pem").toString(); }
QString AppConfig::ffmpegPath() const { return m_settings.value("Media/ffmpegPath", "").toString().trimmed(); }



// ── Meta ──────────────────────────────────────────────────────────────────────
bool AppConfig::isLoaded() const
{
    return m_loaded;
}

void AppConfig::setDdnsUsername(const QString &value) { m_settings.setValue("DDNS/username", value); }

void AppConfig::setDdnsPassword(const QString &value) { m_settings.setValue("DDNS/password", value); }

void AppConfig::setDdnsHostname(const QString &value) { m_settings.setValue("DDNS/hostname", value); }

void AppConfig::setServerPort(quint16 value) { m_settings.setValue("Server/port", value); }

void AppConfig::setNasRoot(const QString &value) { m_settings.setValue("Server/nasRoot", value); }

void AppConfig::setUseHttps(bool value)               { m_settings.setValue("Server/useHttps",  value); }

void AppConfig::setCertPath(const QString& value)     { m_settings.setValue("Server/certPath",  value); }
void AppConfig::setKeyPath(const QString& value)      { m_settings.setValue("Server/keyPath",   value); }

bool AppConfig::save()
{
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}