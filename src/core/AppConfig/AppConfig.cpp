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
    ensureDefaults();
    if (!m_loaded)
        qDebug() << "[Config] ⚠️ config.ini not found at" << configPath()
                 << "— using defaults.";
}

/**
 * @brief Ensures that all expected keys exist in the config file, setting defaults if missing.
 *
 * This is called during construction to guarantee that getters always have valid data to return,
 * even if the user hasn't created a config.ini yet. It checks for each key and sets it to a
 * default value if it's not already present. Finally, it calls sync() to write any changes back
 * to disk immediately.
 */
void AppConfig::ensureDefaults()
{
    auto setIf = [this](const QString& key, const QVariant& def) {
        if (m_settings.value(key).isNull())
            m_settings.setValue(key, def);
    };

    setIf("Server/port",     9090);
    setIf("Server/nasRoot",  "E:/");
    setIf("Server/useHttps", false);
    setIf("Server/certPath", "cert.pem");
    setIf("Server/keyPath",  "key.pem");
    m_settings.sync();
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