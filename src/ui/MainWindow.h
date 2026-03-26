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

// —— Our Own Module Includes —————————————————————————————————————————————————————————————————

#include "core/FileServer/FileServer.h"     // HTTP file server — serves files over LAN/internet
                            // MainWindow creates and owns one instance of this.

#include "core/DdnsUpdater/DdnsUpdater.h"    // DDNS auto-updater — keeps hostname pointing to current IP
                            // to No-IP so nestdrive.ddns.net always resolves
                            // MainWindow creates and owns one instance of this.

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
    explicit MainWindow(QWidget* parent = nullptr);

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

private:
    // ── UI Widget Members ──────────────────────────────────────────────────────
    // Initialized to nullptr so they have a safe default value before
    // setupUI() assigns real objects to them.

    QTreeView*        m_fileTree   = nullptr; // The file browser tree — shows E:/ contents
    QFileSystemModel* m_fileModel  = nullptr; // Supplies live filesystem data to m_fileTree
    QPushButton*      m_serverBtn  = nullptr; // "▶ Start Server" / "■ Stop Server" toggle button
    QLabel*           m_serverStatus = nullptr; // Shows server URL or "Server stopped" text

    // ── Core Component Members ─────────────────────────────────────────────────
    // Both are heap-allocated with 'this' as Qt parent, so they are automatically
    // destroyed when MainWindow is destroyed — no manual delete needed.

    FileServer*   m_fileServer   = nullptr; // Owns the HTTP server lifecycle
    DdnsUpdater*  m_ddnsUpdater  = nullptr; // Owns the DDNS polling timer and API calls

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
