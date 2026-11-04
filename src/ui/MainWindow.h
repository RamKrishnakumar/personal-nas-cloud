#pragma once

/**
 * @file MainWindow.h
 * @brief Declares the main application window for NestDrive.
 *
 * MainWindow is the central hub of the app. It owns:
 *  - The file browser UI (QTreeView + QFileSystemModel)
 *  - The server control bar (QPushButton + QLabel)
 *  - The FileServer instance (handles all HTTP file serving)
 *  - The DdnsUpdater instance (keeps DDNS hostname in sync with WAN IP)
 */

#include <QMainWindow>      // Base class for main application windows
                            // Provide the title bar, menu bar, and central widget area

#include <QTreeView>        // Widget that displays files in a tree structure
                            // We use this to show the E:/ drive contents as a file browser

#include <QFileSystemModel> // Provides live filesystem data to QTreeView
                            // It watches the directory and auto-refreshes when files changes

#include <QLabel>           // Displays static or dynamic text
                            // Used to show "Server Stopped" or the liver URL when running.

#include <QPushButton>      // Clickable button widget
                            // Used for the Start/Stop Server toggle button.

#include <QComboBox>        // Drop-down list widget
                            // Used to choose how the library is reached from outside.

// —— Our Own Module Includes —————————————————————————————————————————————————————————————————

#include "core/FileServer/FileServer.h"     // HTTP file server — serves files over LAN/internet
                            // MainWindow creates and owns one instance of this.

#include "core/DdnsUpdater/DdnsUpdater.h"    // DDNS auto-updater — keeps hostname pointing to current IP
                            // to No-IP so nestdrive.ddns.net always resolves
                            // Only useful with a real public IP — see docs/REMOTE-ACCESS.md

#include "core/Tls/CertRenewer.h"   // Keeps a real certificate from expiring, without a restart
#include "core/Tls/Tailscale.h"     // Asks the local Tailscale node for its name and a certificate
#include "core/Tls/TlsConfig.h"     // resolveTlsPath(), so this reads the file the server serves

#include "core/Net/ConnectionCheck.h"   // Public address, or behind the provider's NAT?
#include "core/Net/Upnp.h"              // Asks the router directly, and can forward a port
#include "core/Remote/RemoteMethod.h"   // Which of the two paths to use

#include <memory>
                            // MainWindow creates and owns one instance of this.

namespace db { class Database; }

#include "core/AppConfig/AppConfig.h"      // Reads credentials and settings from config.ini at runtime
                            // this is how we aviod hardcoding passwords in source code.
                            // config.ini lives at project root and is listed in .gitignore.

class MainWindow : public QMainWindow {
    // Q_OBJECT macro is mandatory for any Qt class that uses signals and slots.
    // It tells Qt's MOC (Meta-Object Compiler) to generate the signal/slot
    // connection plumbing for this class behind the scenes.
    Q_OBJECT

public:
    /**
     * @brief Constructs the main window, initializes all components,
     *        and builds the UI layout.
     * @param parent  Qt parent widget (nullptr = top-level window)
     */
    /**
     * @param database Used for the thumbnail index. May be null, in which case
     *                 thumbnails are disabled and the grid links originals.
     */
    explicit MainWindow(db::Database* database = nullptr, QWidget* parent = nullptr);

private slots:
    /**
     * @brief Toggles the HTTP file server on or off when button is clicked.
     * Connected to: m_serverBtn clicked signal
     */
    void onToggleServer();

    /**
     * @brief Updates UI to show green "Running at http://..." status.
     * Connected to: FileServer::serverStarted signal
     */
    void onServerStarted(QString ip, quint16 port);

    /**
     * @brief Resets UI back to grey "Server stopped" status.
     * Connected to: FileServer::serverStopped signal
     */
    void onServerStopped();

    /**
     * @brief Opens the account dialog.
     *
     * Also called once at startup when no account exists, which is the only time
     * the dialog refuses to be dismissed.
     */
    void onManageAccounts();

    /**
     * @brief Shows how to reach the library from outside the house.
     *
     * Reports what Tailscale is doing, the address to use, and the certificate's
     * expiry; explains the setup when there is nothing to report. Deliberately a
     * dialog rather than the start of a settings wizard — the roadmap's v0.5 wizard
     * does not exist, and remote access should not be the thing that invents it.
     */
    void onShowRemoteAccess();

    /** Saves the chosen remote-access method and re-runs setup for it. */
    void onRemoteMethodChanged(int index);

    /** Asks the router to forward the server's port to this machine. */
    void onForwardPort();

private:
    /** Prompts for the administrator account when none exists yet. */
    void promptForFirstAdminIfNeeded();

    /**
     * @brief Logs and audits a newly created account. A no-op when none was.
     *
     * Shared by both entry points, because an account created from the Accounts
     * button is exactly as worth recording as the first one.
     */
    void recordAccountCreated(int userId, const QString& username);

    /**
     * @brief Works out how this machine is reachable, and prepares that path.
     *
     * Re-runnable: tears down whatever the last run built before starting again, so
     * changing the method in the control bar does not need a restart.
     *
     * Probing Tailscale is immediate, but classifying the connection is two network
     * round trips, so the second half finishes in applyRemoteMethod() once
     * core::ConnectionCheck reports. Nothing here blocks the window.
     */
    void setUpRemoteAccess();

    /**
     * @brief Finishes setup once the connection type is known.
     *
     * Resolves the effective method, arranges certificate handling for it, and brings
     * the window's labels into line.
     */
    void applyRemoteMethod(core::ConnectionType connection);

    /**
     * @brief Starts the DDNS updater, if publishing an address is the right thing.
     *
     * Called from applyRemoteMethod() rather than the constructor, because the
     * decision needs the connection type: behind the provider's NAT the visible
     * address belongs to the provider and is shared, so publishing it would aim this
     * household's hostname at somebody else's connection.
     */
    void startDdnsIfUseful();

    /** Certificate renewal through Tailscale. Only for the Tailscale method. */
    void startTailscaleCertificates();

    /**
     * @brief Checks, but never replaces, the certificate for the public method.
     *
     * The distinction matters: on this path the certificate is the owner's, obtained
     * with win-acme or similar, and renewed by that tool's own scheduled task.
     * Overwriting it would undo their setup.
     */
    void checkPublicCertificate();

    /**
     * @brief The URL to reach this server from outside, or empty if there is none.
     *
     * Built from a name rather than an address wherever possible, because the
     * certificate is issued for the name — the MagicDNS name for the Tailscale
     * method, the DDNS hostname for the public one.
     */
    QString remoteUrl() const;

    /** "https" or "http", matching how the server was actually started. */
    QString scheme() const;

    // ── UI Widget Members ──────────────────────────────────────────────────────
    // Initialized to nullptr so they have a safe default value before
    // setupUI() assigns real objects to them.
    
    void loadConfig(); // Helper method to load settings from config.ini — called once in constructor

    QTreeView*        m_fileTree   = nullptr; // The file browser tree — shows E:/ contents
    QFileSystemModel* m_fileModel  = nullptr; // Supplies live filesystem data to m_fileTree
    QPushButton*      m_serverBtn  = nullptr; // "▶ Start Server" / "■ Stop Server" toggle button
    QLabel*           m_serverStatus = nullptr; // Shows server URL or "Server stopped" text
    QPushButton*      m_accountsBtn  = nullptr; // Opens the account dialog
    QLabel*           m_accountWarning = nullptr; // Shown while no account exists
    QPushButton*      m_remoteBtn    = nullptr; // Opens the remote access dialog
    QLabel*           m_certWarning  = nullptr; // Shown while the certificate is near expiry
    QComboBox*        m_remoteMethodBox = nullptr; // Auto / Tailscale / Public link / LAN

    // ── Core Component Members ─────────────────────────────────────────────────
    // Both are heap-allocated with 'this' as Qt parent, so they are automatically
    // destroyed when MainWindow is destroyed — no manual delete needed.

    FileServer*   m_fileServer   = nullptr; // Owns the HTTP server lifecycle
    DdnsUpdater*  m_ddnsUpdater  = nullptr; // Owns the DDNS polling timer and API calls
    db::Database* m_database = nullptr;  // Not owned; outlives this window
    AppConfig     m_config;                // Loads settings from config.ini at startup

    // ── Remote access ──────────────────────────────────────────────────────────
    // unique_ptr rather than Qt parenting because core::Tailscale is not a QObject,
    // and the renewer has to be destroyed before it so its last call cannot outlive
    // what it calls into.
    std::unique_ptr<core::Tailscale>      m_tailscale;
    std::unique_ptr<core::CertRenewer>    m_certRenewer;
    std::unique_ptr<core::UpnpClient>     m_upnp;
    std::unique_ptr<core::ConnectionCheck> m_connectionCheck;

    /// MagicDNS name from the last status query, empty when there is none.
    QString m_remoteName;

    /// What detection concluded, and which path is actually in use.
    core::ConnectionType m_connection = core::ConnectionType::Unknown;
    core::RemoteMethod   m_method     = core::RemoteMethod::LanOnly;

    /// Why the effective method differs from what was configured, or empty.
    QString m_methodNote;

    // ── Private Helper Method ──────────────────────────────────────────────────

    /**
     * @brief Builds and arranges all UI widgets inside the window.
     *
     * Kept separate from the constructor to keep the constructor clean and readable.
     * Called once at the very end of the constructor, after all members are initialized.
     *
     * Creates this layout structure:
     *   QMainWindow
     *   └── central QWidget
     *       └── QVBoxLayout  (vertical stack)
     *           ├── QHBoxLayout  (horizontal control bar)
     *           │   ├── m_serverBtn     (Start/Stop button)
     *           │   └── m_serverStatus  (URL or stopped label)
     *           └── m_fileTree  (file browser — fills remaining space)
     */
    void setupUI();
};
