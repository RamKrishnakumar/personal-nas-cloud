#include "AppConfig.h"

#include <QCoreApplication>
#include <QFile>
#include <QDebug>

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
    : m_settings(
          QCoreApplication::applicationDirPath() + "/config.ini",
          QSettings::IniFormat   // Plain text key=value file format
      )
    , m_loaded(false)
{
    // Check if the config file actually exists
    QString configPath = QCoreApplication::applicationDirPath() + "/config.ini";

    if (!QFile::exists(configPath)) {
        qDebug() << "[Config] ⚠️  config.ini not found at:" << configPath;
        qDebug() << "[Config] Copy config.ini.example → config.ini and fill in your credentials.";
        m_loaded = false;
        return;
    }

    m_loaded = true;
    qDebug() << "[Config] ✅ Loaded config.ini from:" << configPath;
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

bool AppConfig::isLoaded() const
{
    return m_loaded;
}
