#pragma once

/**
 * @file DdnsUpdater.h
 * @brief Automatically keeps a No-IP DDNS hostname pointing to the current WAN IP.
 *
 * How it works:
 *  1. A QTimer fires every N milliseconds (typically 5 minutes)
 *  2. Each tick calls updateNow() which fetches the current WAN IP from ipify.org
 *  3. If the IP has changed since last check, it sends an HTTP update to No-IP
 *  4. If the IP is the same, nothing happens — no unnecessary API calls
 *
 * Credentials are never hardcoded here — they are injected via setCredentials()
 * which is called from MainWindow using values read from config.ini.
 */

#include <QObject>                  // Base class for all Qt objects — required for signals/slots
#include <QTimer>                   // Provides the repeating timer that triggers IP checks
#include <QNetworkAccessManager>    // Qt's HTTP engine — handles all GET requests
#include <QString>                  // Qt's Unicode string class

class DdnsUpdater : public QObject {
    // Q_OBJECT is mandatory — it tells Qt's MOC tool to generate
    // signal/slot wiring code for this class at compile time.
    Q_OBJECT

public:
    /**
     * @brief Constructs the updater. Does NOT start the timer yet.
     * Call setCredentials() then start() to activate.
     * @param parent  Qt parent object — handles memory cleanup automatically
     */
    explicit DdnsUpdater(QObject* parent = nullptr);

    /**
     * @brief Stores No-IP credentials for use in update requests.
     *
     * Must be called before start(). Credentials are read from config.ini
     * via AppConfig — never hardcoded in source.
     *
     * @param username  Your No-IP account username
     * @param password  Your No-IP account password
     * @param hostname  The DDNS hostname to update (e.g. nestdrive.ddns.org)
     */
    void setCredentials(const QString& username,
                        const QString& password,
                        const QString& hostname);

    /**
     * @brief Starts the repeating timer and runs an immediate first check.
     *
     * The immediate first check means the hostname is updated at app launch
     * without waiting for the full interval (e.g. 5 minutes) to pass.
     *
     * @param intervalMs  Polling interval in milliseconds (e.g. 5 * 60 * 1000)
     */
    void start(int intervalMs = 5 * 60 * 1000);

    /**
     * @brief Stops the repeating timer.
     * The last known WAN IP is preserved so the next start() call
     * won't send a redundant update if the IP hasn't changed.
     */
    void stop();

signals:
    /**
     * @brief Emitted when No-IP confirms the hostname was updated.
     * Connected in MainWindow.cpp to a qDebug() log lambda.
     * @param ip        The new WAN IP that was registered
     * @param response  The raw response string from No-IP (e.g. "good 1.2.3.4")
     *                  Common values:
     *                  "good 1.2.3.4"  — IP updated successfully
     *                  "nochg 1.2.3.4" — IP unchanged, no update needed
     *                  "nohost"        — Hostname not found in account
     *                  "badauth"       — Wrong username or password
     */
    void updateSucceeded(const QString& ip, const QString& response);

    /**
     * @brief Emitted when any step of the update process fails.
     * Connected in MainWindow.cpp to a qDebug() log lambda.
     * @param error  Human-readable description of what went wrong
     */
    void updateFailed(const QString& error);

private slots:
    /**
     * @brief Runs one full IP-check-and-update cycle.
     *
     * Called automatically by m_timer on every tick.
     * Also called immediately by start() for an instant first update.
     *
     * Flow:
     *   GET ipify.org → read WAN IP → compare with m_lastWanIp
     *   → if changed: GET No-IP update URL → parse response → emit signal
     *   → if same: do nothing
     */
    void updateNow();

private:
    // ── Credentials (loaded from config.ini, never hardcoded) ─────────────────
    QString m_username;   ///< No-IP account username
    QString m_password;   ///< No-IP account password
    QString m_hostname;   ///< DDNS hostname to keep updated (e.g. nestdrive.ddns.org)

    // ── State ─────────────────────────────────────────────────────────────────
    QString m_lastWanIp;  ///< Last WAN IP we successfully pushed to No-IP.
    ///< Empty string on first run — guarantees first update fires.

    // ── Qt Infrastructure ─────────────────────────────────────────────────────
    QTimer*                  m_timer; ///< Fires every intervalMs — triggers updateNow()
    QNetworkAccessManager   m_net;  ///< Qt's HTTP engine — reused for all requests.
    ///< Single instance is more efficient than creating
    ///< a new one per request (it manages connection pools).
};
