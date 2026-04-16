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
    const QString appDirPath = QCoreApplication::applicationDirPath() + "/config.ini"; // Look for config.ini next to the executable first
    if (QFile::exists(appDirPath))
    {
        return appDirPath; // Found config.ini next to the executable
    }

    const QString localPath = QDir::currentPath() + "/config.ini"; // Fallback: look for config.ini in the current working directory (where the app was launched from)
    if (QFile::exists(localPath))
        return localPath; // Found config.ini in the current working directory

    return appDirPath; // Default to app directory path even if it doesn't exist, so the warning message is consistent.
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
    : m_settings(
          configPath(), QSettings::IniFormat),
      m_loaded(QFile::exists(configPath()))
{
    ensureDefaults();
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
    if (m_settings.value("Server/port").isNull())
        m_settings.setValue("Server/port", 9090);
    if (m_settings.value("Server/nasRoot").isNull())
        m_settings.setValue("Server/nasRoot", "E:/");
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

bool AppConfig::isLoaded() const
{
    return m_loaded;
}

void AppConfig::setDdnsUsername(const QString &value) { m_settings.setValue("DDNS/username", value); }

void AppConfig::setDdnsPassword(const QString &value) { m_settings.setValue("DDNS/password", value); }

void AppConfig::setDdnsHostname(const QString &value) { m_settings.setValue("DDNS/hostname", value); }

void AppConfig::setServerPort(quint16 value) { m_settings.setValue("Server/port", value); }

void AppConfig::setNasRoot(const QString &value) { m_settings.setValue("Server/nasRoot", value); }

bool AppConfig::save()
{
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}