# NestDrive — Product Roadmap

> A personal self-hosted NAS (Network Attached Storage) built with Qt6 (C++).
> Runs on Windows. Accessible from any browser on the network or internet.
> Designed to be generic — any user can install and configure it for their setup.

---

## Version History

| Version | Status | Branch | Description |
|---------|--------|--------|-------------|
| v0.1 | ✅ Released | main | Basic file server over HTTP on LAN |
| v0.2 | ✅ Released | main | File browser UI with QTreeView |
| v0.3 | ✅ Released | main | System tray integration (TrayManager stub) |
| v0.4 | ✅ Released | main | DDNS auto-updater (No-IP), AppConfig, config.ini |

---

## Upcoming Versions

---

### v0.5 — Settings & Setup Wizard
**Branch:** `feature/phase1-settings` → `develop` → `main`
**Goal:** Make NestDrive generic — any user can configure it without touching code.

#### Features
- [ ] Tabbed `SettingsDialog` window with gear icon in MainWindow toolbar
- [ ] **Server Tab**
  - [ ] Drive/folder picker using QFileDialog (replaces hardcoded `E:/`)
  - [ ] Server port selector (replaces hardcoded `8080`)
  - [ ] All values saved to and read from `config.ini`
- [ ] **DDNS Tab**
  - [ ] Input fields for No-IP username, password, hostname
  - [ ] Test button to trigger an immediate DDNS update and show result
  - [ ] Saved to `config.ini`
- [ ] **Setup Guide Tab**
  - [ ] Step-by-step instructions for Windows Firewall rule (copy-paste command pre-filled)
  - [ ] Step-by-step instructions for router port forwarding
  - [ ] Port number in commands auto-updates when user changes port in Server Tab
- [ ] **First-Run Detection**
  - [ ] If `config.ini` is missing or incomplete on launch → show SettingsDialog automatically
  - [ ] App does not start server until minimum config is filled in

---

### v0.6 — Authentication & User Roles
**Branch:** `feature/phase2-auth` → `develop` → `main`
**Goal:** Secure the file server — no unauthenticated access allowed.

#### Features
- [ ] Login page served by FileServer before any file content
- [ ] Session token system (cookie-based, expires after inactivity)
- [ ] `users.json` file stores all user accounts (never committed to GitHub)
- [ ] Password hashing (bcrypt or Qt-based SHA-256 + salt)
- [ ] Three built-in roles:

| Role | Read | Write/Upload | Create Folder | Delete Own Files | Settings |
|------|------|-------------|---------------|-----------------|----------|
| admin | ✅ All | ✅ All | ✅ | ✅ All | ✅ |
| family | ✅ All | ✅ All | ✅ | ✅ Own only | ❌ |
| guest | ✅ Listed folders only | ❌ Unless granted | ✅ If write granted | ❌ | ❌ |

- [ ] Per-folder permission overrides per user (`read / write / none`)
- [ ] Guest users only see folders explicitly listed in their profile
- [ ] Own-file delete — file tagged with `uploaded_by` at upload time
- [ ] "New Folder" button hidden if user lacks write permission on current path

---

### v0.7 — Remote Admin Panel
**Branch:** `feature/phase3-admin` → `develop` → `main`
**Goal:** Admin can fully manage users and monitor the server from any browser remotely.

#### Features
- [ ] Admin-only web panel (separate route, requires admin session)
- [ ] **User Management**
  - [ ] Create new user (username, password, role)
  - [ ] Assign per-folder permissions with folder browser
  - [ ] Edit existing user permissions
  - [ ] Disable/delete user
  - [ ] Grant/revoke guest upload permission per folder
- [ ] **Live Status Dashboard**
  - [ ] Server uptime
  - [ ] Current WAN IP + last DDNS update timestamp
  - [ ] Currently connected clients (IP, username, active file)
- [ ] **Access Log / Reports**
  - [ ] Table: `timestamp | username | action | file path`
  - [ ] Actions: login, logout, download, upload, delete, create folder
  - [ ] Filterable by user and date range
  - [ ] Exportable as CSV

---

### v0.8 — File Operations
**Branch:** `feature/phase4-fileops` → `develop` → `main`
**Goal:** Full file management from the browser, respecting all role permissions.

#### Features
- [ ] File upload from browser (drag and drop + file picker)
- [ ] Upload progress bar for large files
- [ ] Create new folder from browser (respects write permission)
- [ ] Download progress indicator
- [ ] Mobile-responsive browser UI
- [ ] Preview support for images and text files in browser

---

### v0.9 — System Integration & Polish
**Branch:** `feature/phase5-system` → `develop` → `main`
**Goal:** NestDrive behaves like a proper Windows application.

#### Features
- [ ] Windows startup option (register/unregister via Settings)
- [ ] System tray — minimize to tray instead of closing
- [ ] Tray right-click menu: Start Server / Stop Server / Open / Quit
- [ ] Tray tooltip shows server status + current WAN IP
- [ ] Bandwidth monitor (MB/s in and out) in status bar
- [ ] Auto-update checker — notifies when a new version is available on GitHub

---

### v1.0 — Public Release
**Branch:** `develop` → `main` (tagged `v1.0`)
**Goal:** Stable, documented, installable by anyone.

#### Features
- [ ] Windows installer (NSIS or Qt Installer Framework)
- [ ] Complete README with screenshots
- [ ] This ROADMAP.md fully checked off through v0.9
- [ ] All known bugs resolved
- [ ] Tested on Windows 10 and Windows 11

---

## Technical Stack

| Component | Technology |
|-----------|-----------|
| Language | C++17 |
| UI Framework | Qt6 Widgets |
| HTTP Server | Qt6 Network (QTcpServer) |
| DDNS Client | Qt6 Network (QNetworkAccessManager) |
| Config Storage | QSettings (INI format) |
| User Storage | JSON (QJsonDocument) |
| Build System | CMake 3.16+ |
| Compiler | MinGW 64-bit (Windows) |

---

## File Structure

