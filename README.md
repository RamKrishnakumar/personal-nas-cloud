# MyNAS-Nestdrive

A cross-platform, native desktop NAS (Network Attached Storage) application built with **C++17 and Qt6**.
Runs directly on the OS with zero browser dependency — lightweight, fast, and privacy-first.

---

## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Tech Stack](#tech-stack)
- [Project Structure](#project-structure)
- [Install a release](#install-a-release)
- [Platform support](#platform-support)
- [Getting Started](#getting-started)
- [Build Instructions](#build-instructions)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

NestDrive-NAS turns your always-on laptop or PC into a personal Network Attached Storage system.
All your files are accessible from anywhere — no cloud subscriptions, no third-party servers,
full control over your own data.

Future versions will include AI-powered photo management (blur detection, face clustering)
using locally running Ollama Vision models — completely offline and private.

---

## Features

### Currently Available (v0.1)
- [x] Native file browser pointing to dedicated storage drive
- [x] System tray daemon — app stays alive in background when window is closed
- [x] Double-click tray icon to reopen window
- [x] Quit from tray context menu

### Coming Soon
- [ ] LAN file server — access files over local network
- [ ] Port forwarding + DDNS — access from anywhere via internet
- [ ] Settings panel — configure NAS root folder, port, and network options
- [ ] User authentication — JWT-based login for remote access
- [ ] AI Photo Management — blur detection via OpenCV
- [ ] Face clustering — group photos by person using Ollama Vision + ChromaDB
- [ ] Semantic photo search — "Show me beach photos" using local LLM
- [x] Linux and macOS builds — packaged by CI, not yet verified on real hardware

---

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++17 |
| UI Framework | Qt6 Widgets |
| Build System | CMake 3.16+ |
| File I/O | Qt QFileSystemModel + std::filesystem |
| Networking (upcoming) | Qt Network → Boost.ASIO (Phase 2) |
| Database (upcoming) | Qt SQL + SQLite |
| AI Layer (upcoming) | OpenCV + Ollama Vision (Llama 3.2) |
| Compiler (Windows) | MinGW 64-bit / MSVC2022 |

---

## Project Structure

MyNAS/
├── CMakeLists.txt # CMake build configuration
├── README.md # Project documentation
├── .gitignore # Git ignore rules
└── src/
├── main.cpp # Application entry point
└── ui/
├── MainWindow.h # Main window declaration
├── MainWindow.cpp # Main window — file browser UI
├── TrayManager.h # System tray declaration
└── TrayManager.cpp # System tray icon and context menu


> Architecture is intentionally modular — UI, networking, database, and AI layers
> are kept separate so each component can be swapped independently as the project scales.

## Configuration

NestDrive reads credentiaons from a local `config.ini` file that is 
**never commited to GitHub**.

### Setup

1. Copy the exmaple config: cp config.ini.example config.ini

2. Edit `config.ini` with your real values:
- No-IP username and password
- Your DDNS hostname
- NAS root drive path

3. Place `config.ini` next to `MyNAS.exe` in the build folder too
4. Run the app - it loads credentials automatically at startup

---

## Install a release

Download from the [Releases](https://github.com/RamKrishnakumar/personal-nas-cloud/releases)
page. Nothing needs to be built.

| Platform | File | Notes |
|---|---|---|
| **Windows 10/11 (x64)** | `NestDrive-<version>-windows-x64.exe` | Installer. A portable `.zip` is published alongside it. SmartScreen warns because the binary is unsigned — **More info → Run anyway**. |
| **macOS (Apple Silicon)** | `NestDrive-<version>-macos-arm64.dmg` | Open it and drag NestDrive to Applications. First launch: **right-click → Open**, once. |
| **Linux (x86_64)** | `NestDrive-<version>-linux-x86_64.AppImage` | `chmod +x` and run. No installation. |

Video features need `ffmpeg` on `PATH`; photos, browsing and sharing work without it.
Reaching your library from outside the house is set up in the app under
**Remote access…** — see [docs/REMOTE-ACCESS.md](docs/REMOTE-ACCESS.md).

Both warnings are because the packages are not code-signed, which costs a yearly
certificate rather than a code change.
[docs/CI-CD.md §5](docs/CI-CD.md) explains exactly what signing would involve.

---

## Platform support

| Platform | Builds | Tested on real hardware |
|---|---|---|
| Windows 10/11 x64 | MinGW 13.1 and MSVC 14.50 | yes — this is the development machine |
| Linux x86_64 | GCC, in CI | **no** |
| macOS Apple Silicon | Clang, in CI | **no** |

The platform-specific code — memory and disk detection in
[HardwareTier.cpp](src/http/HardwareTier.cpp), battery detection in
[ThumbnailScanner.cpp](src/media/ThumbnailScanner.cpp), case-sensitivity in
[PathResolver.cpp](src/http/PathResolver.cpp) — has real branches for all three, but
the Linux and macOS ones are compiled by CI and have never been exercised by a person.
Treat those two as *builds and should work* rather than *verified*.

---

## Getting Started

### Prerequisites

| Tool | Version | Download |
|---|---|---|
| Qt6 + Qt Creator | 6.x or later | [qt.io/download](https://www.qt.io/download-open-source) |
| CMake | 3.16+ | Bundled with Qt installer |
| Compiler | MinGW 64-bit or MSVC2022 | Bundled with Qt (MinGW) |

### Installation

1. Clone the repository:
```bash
git clone https://github.com/YourUsername/MyNAS.git
cd MyNAS

```

2. Open in Qt Creator:

    File → Open File or Project

    Select CMakeLists.txt

    Choose kit: Desktop Qt 6.x.x MinGW 64-bit

    Click Configure Project

3. Build and Run:

    Press Ctrl+R or click the green Run button


### Build Instructions
Using Qt Creator (Recommended)

File → Open → CMakeLists.txt → Configure → Ctrl+R

Using Command Line

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The binary will be output to build/ or build/Release/.

### Tests

```bash
ctest --test-dir build --output-on-failure -j 2
```

33 suites. Two notes that cost real time to rediscover:

- **On Windows the Qt DLLs must be on `PATH`**, or every suite exits `0xc0000135`
  (`STATUS_DLL_NOT_FOUND`) with no output at all —
  `export PATH="/c/Qt/6.10.3/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"`.
- **`-j 2`, not more.** A dozen suites bind real TCP sockets on localhost and start
  competing for ports above that.

### Building a package yourself

```bash
cmake --install build --prefix dist
```

That produces a runnable tree: on Windows the executable with its Qt DLLs and plugins
beside it, on macOS a deployed `MyNAS.app`. On Linux it produces an FHS tree that
`linuxdeploy` turns into an AppImage.

CI does this on all three platforms on every push, and publishes installers from the
`production` branch. See [docs/CI-CD.md](docs/CI-CD.md).

### Roadmap

v0.1  ✅  Native file browser + system tray (current)
v0.2  ✅  Background HTTP file server — LAN access
v0.3  ✅  Port forwarding + DDNS config in Settings UI
v0.4  ✅ Released | main | DDNS auto-updater (No-IP), AppConfig, config.ini |
More details you will find under Roadmap.md file[Roadmap.md]

### Contributing

This project is in active early development. Contributions, suggestions,
and issue reports are welcome.

    Fork the repository

    Create a feature branch: git checkout -b feature/your-feature

    Commit your changes: git commit -m "Add your feature"

    Push to the branch: git push origin feature/your-feature

    Open a Pull Request

### License

This project is licensed under the MIT License — free to use, modify, and distribute.

    Built by Ram Krishna — a personal project to learn
    systems programming, cross-platform C++, and local AI integration.
