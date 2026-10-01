/**
 * @file MainWindow.cpp
 * @brief Main application window for NestDrive.
 *
 * This file is responsible for:
 *  1. Building the entire UI layout (server control bar + file browser)
 *  2. Owning and managing the FileServer instance (LAN/remote file access)
 *  3. Owning and managing the DdnsUpdater instance (keeps DDNS hostname current)
 *  4. Reacting to server start/stop events and updating UI labels accordingly
 */

// ── Includes ──────────────────────────────────────────────────────────────────
#include "MainWindow.h"                 // This file's own header — declares the class

#include <QVBoxLayout>                  // Vertical box layout — stacks widgets top to bottom
#include <QHBoxLayout>                  // Horizontal box layout — places widgets side by side
#include <QWidget>                      // Base class for all UI elements
#include <QHeaderView>                  // Controls the column header behaviour in QTreeView
#include <QDir>                         // Filesystem directory operations (checking if E:/ exists)
#include <QDebug>                       // qDebug() for logging to Qt Creator's output panel
#include <QMessageBox>                  // Confirmation after an account is created

#include "app/Paths.h"
#include "auth/UserStore.h"             // Account lookup and creation
#include "ui/AccountDialog.h"           // Collects a new account's details

// ── Constructor ───────────────────────────────────────────────────────────────

/**
 * @brief Constructs the MainWindow.
 *
 * Order of operations:
 *  1. Initialize member objects (FileServer, DdnsUpdater) via initializer list
 *  2. Set window title and size
 *  3. Connect FileServer signals to our UI update slots
 *  4. Initialize and start the DdnsUpdater
 *  5. Build the UI layout (setupUI)
 *
 * @param parent  Qt parent widget — passing nullptr makes this a top-level window
 */
MainWindow::MainWindow(db::Database* database, QWidget* parent)
    : QMainWindow(parent)                    // Initialize Qt's base window class
    , m_database(database)                   // Not owned — created in main(), outlives this window
    , m_fileServer(new FileServer(this))     // Create file server — 'this' = Qt parent (auto memory managed)
    , m_ddnsUpdater(new DdnsUpdater(this))   // Create DDNS updater — 'this' = Qt parent (auto memory managed)
{
    // ── Window Setup ──────────────────────────────────────────────────────────
    setWindowTitle("NestDrive v0.5");  // Title shown in the window title bar
    resize(900, 650);                  // Default window size in pixels (width x height)

    // ── FileServer Signal Connections ─────────────────────────────────────────
    // Qt signals & slots: when FileServer emits a signal, our slot function
    // is called automatically. This keeps server logic decoupled from UI logic.

    // When server successfully starts → call onServerStarted() to update the UI label
    connect(m_fileServer, &FileServer::serverStarted,
            this,          &MainWindow::onServerStarted);

    // When server stops → call onServerStopped() to reset the UI label
    connect(m_fileServer, &FileServer::serverStopped,
            this,          &MainWindow::onServerStopped);

    // ── DDNS Updater Setup ────────────────────────────────────────────────────
    // The DDNS updater runs silently in the background every 5 minutes.
    // It checks if the WAN IP has changed and pushes an update to No-IP
    // so that nestdrive.ddns.org always resolves to the correct IP.
    // This ensures remote access works even after Airtel changes the public IP.

    // Credentials come from config.ini via m_config — never hardcoded in source.
    const QString ddnsUser = m_config.ddnsUsername();
    const QString ddnsPass = m_config.ddnsPassword();
    const QString ddnsHost = m_config.ddnsHostname();

    if (ddnsUser.isEmpty() || ddnsPass.isEmpty() || ddnsHost.isEmpty()) {
        // Gate on the credentials themselves rather than on isLoaded(). A
        // config.ini written on first launch exists but has blank DDNS fields,
        // and starting the updater then would send a failing authentication
        // request to No-IP every five minutes — repeated failures can get an
        // account blocked. LAN and HTTPS access are unaffected.
        qDebug() << "[Main] ℹ️ DDNS credentials incomplete in"
                 << AppConfig::configPath()
                 << "— DDNS disabled. LAN access still works.";
    } else {
        m_ddnsUpdater->setCredentials(ddnsUser, ddnsPass, ddnsHost);
        m_ddnsUpdater->start(5 * 60 * 1000);

        connect(m_ddnsUpdater, &DdnsUpdater::updateSucceeded,
                this, [](const QString& ip, const QString& response) {
            qDebug() << "[DDNS] ✅ Updated. IP:" << ip << "Response:" << response;
        });

        connect(m_ddnsUpdater, &DdnsUpdater::updateFailed,
                this, [](const QString& error) {
            qDebug() << "[DDNS] ❌ Failed:" << error;
        });
    }

    // ── Thumbnails ────────────────────────────────────────────────────────────
    // The grid points at 320 px copies instead of originals, which is what makes a
    // folder of photos usable over a slow link.
    m_fileServer->setThumbnailStore(m_database, app::paths::thumbnailDirectory());

    // ── Accounts ──────────────────────────────────────────────────────────────
    // Serves /login and the session endpoints. It does not yet protect anything:
    // the gate that requires a session on every other route is step 14, and the
    // first-run wizard that creates the admin account is step 16.
    m_fileServer->setAccountStore(m_database);

    // ── Build UI ──────────────────────────────────────────────────────────────
    // Called last — all member objects must be initialized before UI is built
    // because setupUI() references m_fileServer and other members.
    setupUI();

    // After the UI exists, so the dialog has a parent to be modal against.
    promptForFirstAdminIfNeeded();
}

// ── Accounts ──────────────────────────────────────────────────────────────────

void MainWindow::promptForFirstAdminIfNeeded()
{
    auth::UserStore* users = m_fileServer->accountStore();

    // Refreshes the warning banner as well, so it disappears the moment an account
    // exists rather than at the next launch.
    const auto refreshWarning = [this](int accounts) {
        const bool exposed = accounts == 0;
        m_accountWarning->setVisible(exposed);
        if (exposed) {
            m_accountWarning->setText(
                tr("⚠  No account exists, so anyone who can reach this machine's "
                   "port can browse and download every file. Create an account "
                   "before exposing NestDrive beyond your own network."));
        }
    };

    if (!users) {
        m_accountsBtn->setEnabled(false);
        m_accountsBtn->setToolTip(tr("Unavailable: the database did not open."));
        refreshWarning(0);
        return;
    }

    if (users->count() > 0) {
        refreshWarning(users->count());
        return;
    }

    // Shown once, at first launch. Declining is allowed — the server still works
    // without accounts today — but the banner then stays up.
    AccountDialog dialog(users, /*firstAdmin=*/true, this);
    dialog.exec();

    refreshWarning(users->count());

    if (!dialog.createdUsername().isEmpty()) {
        qInfo() << "[Auth] Administrator account created:" << dialog.createdUsername();
    }
}

void MainWindow::onManageAccounts()
{
    auth::UserStore* users = m_fileServer->accountStore();
    if (!users)
        return;

    const bool firstAdmin = users->count() == 0;

    AccountDialog dialog(users, firstAdmin, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_accountWarning->setVisible(users->count() == 0);

    QMessageBox::information(
        this, tr("Account created"),
        tr("%1 can now sign in at %2/login.")
            .arg(dialog.createdUsername(),
                 m_fileServer->isRunning()
                     ? QStringLiteral("http://%1:%2")
                           .arg(m_fileServer->localIP())
                           .arg(m_fileServer->port())
                     : tr("the server address once it is running")));
}

// ── UI Builder ────────────────────────────────────────────────────────────────

/**
 * @brief Constructs and arranges all UI widgets inside the main window.
 *
 * Layout structure:
 *  QMainWindow
 *  └── central QWidget
 *      └── QVBoxLayout (mainLayout) — vertical stack
 *          ├── QHBoxLayout (barLayout) — top control bar
 *          │   ├── QPushButton (m_serverBtn) — start/stop server
 *          │   └── QLabel (m_serverStatus) — shows current server URL
 *          └── QTreeView (m_fileTree) — file browser for E:/ drive
 */
void MainWindow::setupUI()
{
    // Every QMainWindow needs a central widget — it fills the window area.
    // All other widgets are placed inside this central widget.
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    // Main vertical layout — stacks the control bar on top, file tree below.
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(8, 8, 8, 8); // 8px padding on all four sides
    mainLayout->setSpacing(6);                   // 6px gap between child widgets

    // ── Server Control Bar ────────────────────────────────────────────────────
    // A horizontal row at the top containing the Start/Stop button and status label.
    auto* barLayout = new QHBoxLayout();

    // Start/Stop button — clicking toggles the HTTP file server on/off.
    m_serverBtn = new QPushButton("▶  Start Server", this);
    m_serverBtn->setFixedHeight(32); // Fixed height keeps the bar compact
    connect(m_serverBtn, &QPushButton::clicked,
            this,         &MainWindow::onToggleServer);

    // Status label — shows "Server stopped" or the live URL when running.
    // Styled grey when stopped, bright green when running.
    m_serverStatus = new QLabel("Server stopped", this);
    m_serverStatus->setStyleSheet("color: #888;"); // Grey = inactive

    // Accounts button — creating the first one is what makes a login possible.
    m_accountsBtn = new QPushButton("Accounts…", this);
    m_accountsBtn->setFixedHeight(32);
    connect(m_accountsBtn, &QPushButton::clicked,
            this,          &MainWindow::onManageAccounts);

    // Add button and label to the horizontal bar
    barLayout->addWidget(m_serverBtn);
    barLayout->addWidget(m_serverStatus);
    barLayout->addStretch(); // Pushes button and label to the left, leaves right side empty
    barLayout->addWidget(m_accountsBtn);

    // Add the horizontal bar to the top of the main vertical layout
    mainLayout->addLayout(barLayout);

    // Standing warning while the server is open to anyone who can reach the port.
    // Visible in the window rather than only in the log, because the log is not
    // where someone decides whether to forward a port.
    m_accountWarning = new QLabel(this);
    m_accountWarning->setWordWrap(true);
    m_accountWarning->setStyleSheet(
        "background: #4a2000; color: #ffd9a0; padding: 6px; border-radius: 4px;");
    m_accountWarning->setVisible(false);
    mainLayout->addWidget(m_accountWarning);

    // ── File Browser ──────────────────────────────────────────────────────────
    // Shows the contents of the NAS storage drive (E:/) in a tree view.
    // Falls back to the user's home folder if E:/ doesn't exist on this machine.

    QString nasRoot = m_config.nasRoot(); // Read NAS root path from config.ini (e.g. "E:/");
    QDir dir(nasRoot);
    if (!dir.exists()) {
        // E:/ not found — fall back gracefully instead of crashing
        nasRoot = QDir::homePath();
        qDebug() << "[NAS] " << nasRoot << " not found, falling back to:" << nasRoot;
    }

    // QFileSystemModel provides live filesystem data to the tree view.
    // It watches the directory for changes (new files, deletions etc.) automatically.
    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setRootPath(nasRoot); // Sets which directory to start scanning from

    // QTreeView renders the filesystem model as an expandable tree.
    m_fileTree = new QTreeView(this);
    m_fileTree->setModel(m_fileModel);

    // setRootIndex tells the tree to show nasRoot as the top-level folder,
    // hiding all parent directories above it (e.g. C:/, D:/ won't appear)
    m_fileTree->setRootIndex(m_fileModel->index(nasRoot));
    m_fileTree->setColumnWidth(0, 300);                              // Name column width
    m_fileTree->setSortingEnabled(true);                             // Allow column sorting
    m_fileTree->header()->setSectionResizeMode(QHeaderView::Interactive); // User can resize columns

    // Add the file browser below the control bar — takes up remaining space
    mainLayout->addWidget(m_fileTree);
}

// ── Server Toggle Slot ────────────────────────────────────────────────────────

/**
 * @brief Called when the Start/Stop button is clicked.
 *
 * Acts as a toggle — checks current state and either starts or stops the server.
 * The UI update (button text, status label) happens via the serverStarted and
 * serverStopped signals emitted by FileServer, keeping this function simple.
 *
 * What this does:
 *   When the user clicks "▶ Start Server" we now:
 *     1. Read useHttps / certPath / keyPath from config.ini via AppConfig.
 *     2. If useHttps=true, call loadTlsFiles() and abort with a message if
 *        the cert/key files cannot be loaded.
 *     3. Call setUseHttps() to arm HTTPS mode in FileServer.
 *     4. Call start() as before — the server now accepts TLS connections.
 *   onServerStarted now shows https:// in the status label.
 */
void MainWindow::onToggleServer()
{
    if (m_fileServer->isRunning()) {
        m_fileServer->stop();
        return;
    }

    // ── Read config ───────────────────────────────────────────────────────
    m_fileServer->setNasRoot(m_config.nasRoot());

    // ── Step 1: wire TLS if useHttps = true in config.ini ────────────────
    const bool wantHttps = m_config.useHttps();
    m_fileServer->setUseHttps(wantHttps);

    if (wantHttps) {
        const bool tlsOk = m_fileServer->loadTlsFiles(
            m_config.certPath(),   // e.g. "cert.pem" next to exe
            m_config.keyPath()     // e.g. "key.pem"  next to exe
        );

        if (!tlsOk) {
            // Surface the error in the status label so the user
            // can fix the paths in config.ini without digging in logs
            m_serverStatus->setStyleSheet("color: #ff4444; font-weight: bold;");
            m_serverStatus->setText("⚠️ TLS error: " + m_fileServer->tlsErrorString());
            return;
        }
    }

    // ── Start server (HTTP or HTTPS depending on setUseHttps above) ───────
    if (!m_fileServer->start(m_config.serverPort())) {
        m_serverStatus->setStyleSheet("color: #ff4444; font-weight: bold;");
        m_serverStatus->setText("⚠️ Port " +
            QString::number(m_config.serverPort()) + " already in use.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onServerStarted(QString ip, quint16 port)
{
    const QString scheme = m_fileServer->useHttps() ? "https" : "http";
    m_serverStatus->setStyleSheet("color: #00cc44; font-weight: bold;");
    m_serverStatus->setText("● Running at " + scheme + "://" + ip
                            + ":" + QString::number(port));
    m_serverBtn->setText("■ Stop Server");
}

/**
 * @brief Called automatically when FileServer emits serverStopped().
 *
 * Resets the status label and button back to their default stopped state.
 */
void MainWindow::onServerStopped()
{
    // Grey = server is inactive
    m_serverStatus->setStyleSheet("color: #888;");
    m_serverStatus->setText("Server stopped");

    // Change button label back to show start action
    m_serverBtn->setText("▶  Start Server");
}
