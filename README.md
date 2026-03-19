# MyNAS-Nestdrive

A cross-platform, native desktop NAS (Network Attached Storage) application built with **C++17 and Qt6**.
Runs directly on the OS with zero browser dependency — lightweight, fast, and privacy-first.

---

## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Tech Stack](#tech-stack)
- [Project Structure](#project-structure)
- [Getting Started](#getting-started)
- [Build Instructions](#build-instructions)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

MyNAS turns your always-on laptop or PC into a personal Network Attached Storage system.
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
- [ ] Linux support
- [ ] macOS support

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

### Roadmap

v0.1  ✅  Native file browser + system tray (current)
v0.2  🔲  Background HTTP file server — LAN access
v0.3  🔲  Port forwarding + DDNS config in Settings UI
v1.0  🔲  Stable remote NAS — Windows binary release
v1.5  🔲  Blur detection sweep + photo grid view
v2.0  🔲  Ollama Vision face clustering + semantic search
v2.5  🔲  Linux binary release
v3.0  🔲  macOS support

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
