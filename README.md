# Harken Audio Middleware

**English** | [简体中文](README.zh-CN.md)

![License](https://img.shields.io/badge/License-Apache--2.0-green) ![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C) ![Ninja](https://img.shields.io/badge/Ninja-1.10%2B-1A1A1A) ![Python](https://img.shields.io/badge/Python-3.10%2B-3776AB) ![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS-lightgrey) ![Status](https://img.shields.io/badge/Status-In%20Development-orange)

Get the project:

```bash
git clone https://github.com/xindong/harken-studio.git
```

## Introduction

**Harken is an in-house game audio middleware** — a software layer between sound assets and the game runtime. It lets sound designers describe *when, how, and what to play* in the language of sound (rather than code), and the runtime turns those descriptions into waveforms at minimal cost.

### The Four Products

1. **Harken Hub** — the official distribution platform, providing Harken updates and downloads;
2. **Harken Studio** — the designer-facing editor for arranging sounds and authoring playback logic;
3. **Harken Engine** — the core sound engine, the primary deliverable for client and engine integration;
4. **Harken SDK** — the standalone integration package for embedding Harken into game engines;

## Prerequisites

### 1. Install Development Tools

#### Windows

| Tool | Version | Purpose | Download |
|------|---------|---------|----------|
| Visual Studio | 2022+ | Compiler, Windows SDK, debugger | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/) |
| Python | 3.10+ | Project tooling scripts | [python.org/download](https://www.python.org/downloads/) |

> We recommend installing Visual Studio (or just the Visual Studio Build Tools, no IDE required) via the Visual Studio Installer. The **"Desktop development with C++"** workload is required, with at least the following components:
> 1. MSVC build tools for x64/x86
> 2. C++ CMake tools for Windows
> 3. C++ Clang tools for Windows
> 4. Windows 10 SDK / Windows 11 SDK

#### macOS

| Tool | Version | Purpose | Install |
|------|---------|---------|---------|
| Xcode | 26.6+ | Apple Clang compiler and system SDKs | [Apple Developer Downloads](https://developer.apple.com/download/all/) |
| Homebrew | latest | Package manager | [brew.sh](https://brew.sh/) |
| CMake | 3.25+ | Build system generator | `brew install cmake` |
| Ninja | 1.10+ | Build executor | `brew install ninja` |
| Python | 3.10+ | Project tooling scripts | `brew install python` |

### 2. Set Up the Python Environment

#### Windows

```bash
python -m venv .venv
.venv/Scripts/pip install -r requirements.txt
```

#### macOS

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

### 3. Download Dependencies

Install a Qt kit ≥ 6.8.3.

#### Windows

For example, install to `D:\Qt` (MSVC 2022 component):

```bash
.venv/Scripts/aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O D:/Qt -b https://mirror.nju.edu.cn/qt
```

#### macOS

For example, install to `~/Qt`:

```bash
.venv/bin/aqt install-qt mac desktop 6.8.3 clang_64 -O ~/Qt -b https://mirror.nju.edu.cn/qt
```

> You can also install manually with the Qt Online Installer (select the MSVC 2022 component on Windows, the macOS component on macOS).

### 4. Set Environment Variables

#### Windows

1. Add CMake to the PATH environment variable
   > The CMake bundled with the Visual Studio installation works fine
2. Create a **system-level** environment variable:
    - Name: VENUS_QT_DIR
    - Value: the MSVC folder inside the downloaded Qt kit, e.g. D:\Qt\6.8.3\msvc2022_64

Manual path: Settings > System > Advanced system settings > Environment Variables > New (System variables)

#### macOS

1. CMake installed via Homebrew is already on PATH, no extra setup needed
2. Export in your shell profile (default `~/.zshrc`):
    - Name: VENUS_QT_DIR
    - Value: the macos folder inside the downloaded Qt kit, e.g. $HOME/Qt/6.8.3/macos

```bash
export VENUS_QT_DIR="$HOME/Qt/6.8.3/macos"
```

**Restart your terminal or IDE after setting the variables**

### 5. Generate the Build System

CMake presets live at the repository root. From the repository root, run:

#### Windows

```bash
cmake --preset windows-ninja-clang-dev   # Ninja + Clang-cl → build-nc/ (fast compiles, strict asserts)
cmake --preset windows-vs-dev            # Visual Studio 17 2022 → build-vs/ (slower compiles, full debugging)
```

#### macOS

```bash
cmake --preset macos-dev    # Ninja + Apple Clang → build-nc/ (single daily channel, build and debug from the same source)
```

### 6. Build and Test

#### Windows

Two daily channels:

1. **`build-nc` + `Release|Debug|RelWithDebInfo` (preferred)**:

    ```bash
    cmake --build build-nc --config RelWithDebInfo [--target <harken_core|harken_editor|harken_hub>]
    ```

2. **`build-vs` + `Release|Debug|RelWithDebInfo` (secondary)**:

    ```bash
    cmake --build build-vs --config Debug  [--target <harken_core|harken_editor|harken_hub>]
    ```

#### macOS

Single daily channel:

- **`build-nc` + `Release|Debug|RelWithDebInfo`**:

    ```bash
    cmake --build build-nc --config RelWithDebInfo [--target <harken_core|harken_editor|harken_hub>]
    ```

## Legal Documents

- Installation and Use License Agreement and Privacy Policy: [English](USER_TERMS.md) | [Simplified Chinese](USER_TERMS.zh-CN.md)
- Harken Studio Open-Source Release Notice: [English](OPEN_SOURCE_RELEASE_NOTICE.txt) | [Simplified Chinese](OPEN_SOURCE_RELEASE_NOTICE.zh-CN.txt)

These root files are the maintained sources for Hub and Studio releases. Hub and
its installers generate their display formats from the user terms. [LICENSE](LICENSE)
and [NOTICE](NOTICE) retain the open-source license and third-party attributions.

## Project Layout

```
harken/
├── .docs/                                # Global project docs and design notes
│   ├── philosophy/                       # Design maps, product ideology (human-managed)
│   ├── quality/                          # Repo-wide quality standards (human-managed)
│   └── snapshot/                         # Per-system form snapshots and summaries (keep current)
├── .github/                              # GitHub workflow configuration
├── .scripts/                             # Development automation scripts (not source code)
├── (.local/)                             # Local files, not committed
├── (.temp/)                              # Temporary files and caches
├── (.venv/)                              # Python virtual environment
├── (build*/)                             # Build output, compiled artifacts
├── cmake/                                # Build identity injection
├── HarkenEngine/                         # harken_core — the sound engine runtime core
├── HarkenHub/                            # HarkenHub — the standalone launcher
├── HarkenSchema/                         # Independent Editor and Pack JSON contracts
├── HarkenSDK/                            # HarkenSDK — shared engine SDK root (Unity/Harken + Unreal/Harken)
├── HarkenStudio/                         # HarkenEditor — the front-end editor
├── CLAUDE.md | AGENTS.md                 # High-level AI project instructions
├── CMake*                                # CMake project solution
├── requirements.txt                      # Python dependencies
├── OPEN_SOURCE_RELEASE_NOTICE*.txt       # Open-source release notice
├── USER_TERMS*.md                        # Privacy policy and license agreement
└── README*.md | LICENSE | VERSION | NOTICE
```
