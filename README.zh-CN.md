# Harken Audio Middleware

[English](README.md) | **简体中文**

![License](https://img.shields.io/badge/License-Apache--2.0-green) ![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C) ![Ninja](https://img.shields.io/badge/Ninja-1.10%2B-1A1A1A) ![Python](https://img.shields.io/badge/Python-3.10%2B-3776AB) ![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS-lightgrey) ![Status](https://img.shields.io/badge/Status-In%20Development-orange)

本项目获取途径: 

```bash
git clone https://github.com/xindong/harken-studio.git
```

## 项目介绍

**Harken 是一套自研的游戏音频中间件**——介于"声音素材"与"游戏运行时"之间的一层软件，让声音设计师用声音的语言（而非代码）描述"什么时候、以什么方式、发出什么声音"，再由运行时以极低开销把这些描述变成波形。

### 四大件产品

1. **Harken Studio Hub**——官方产品分发平台，提供 Harken Studio 的更新与下载；
2. **Harken Studio**——设计师编辑器，编排设计声音、规划发声逻辑；
3. **Harken Engine**——核心声音引擎，客户端以及引擎集成的核心产物；
4. **Harken SDK**——独立集成包，用于游戏引擎集成 Harken；

## 项目部署要求

### 1. 安装开发工具

#### Windows

| 工具 | 版本 | 用途 | 下载地址 |
|------|------|------|-------|
| Visual Studio | 2022+ | 编译器、Windows SDK、调试器 | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/) |
| Python | 3.10+ | 项目工具脚本运行 | [python.org/download](https://www.python.org/downloads/) |

> 建议使用 Visual Studio Installer 安装 Visual Studio 或仅安装 Visual Studio Build Tools (不需要 IDE)，必须勾选 **"使用 C++ 的桌面开发"** 工作负荷，并至少选择以下选项:
> 1. 适用于 x64/x86 的 MSVC 生成工具
> 2. 用于 Windows 的 C++ CMake 工具
> 3. 用于 Windows 的 C++ Clang 工具
> 4. Windows 10 SDK / Windows 11 SDK

#### macOS

| 工具 | 版本 | 用途 | 安装方式 |
|------|------|------|-------|
| Xcode | 26.6+ | Apple Clang 编译器与系统 SDK | [Apple Developer 下载](https://developer.apple.com/download/all/) |
| Homebrew | latest | 包管理器 | [brew.sh](https://brew.sh/) |
| CMake | 3.25+ | 构建系统生成器 | `brew install cmake` |
| Ninja | 1.10+ | 构建执行器 | `brew install ninja` |
| Python | 3.10+ | 项目工具脚本运行 | `brew install python` |

### 2. 配置 Python 环境

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

### 3. 下载依赖

安装 Qt Kit ≥ 6.8.3。

#### Windows

例如安装到 `D:\Qt`(MSVC 2022 组件):

```bash
.venv/Scripts/aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O D:/Qt -b https://mirror.nju.edu.cn/qt
```

#### macOS

例如安装到 `~/Qt`:

```bash
.venv/bin/aqt install-qt mac desktop 6.8.3 clang_64 -O ~/Qt -b https://mirror.nju.edu.cn/qt
```

> 也可用 Qt Online Installer 手动安装(Windows 勾选 MSVC 2022 组件,macOS 勾选 macOS 组件)。

### 4. 设置环境变量

#### Windows

1. 将 CMake 写入 PATH 环境变量
   > CMake 可用安装 Visual Studio 时附带安装的路径
2. 创建**系统级**环境变量:
    - 变量名:VENUS_QT_DIR
    - 变量值:所下载的 Qt 中的 MSVC 文件夹, 例如 D:\Qt\6.8.3\msvc2022_64

手动操作路径:设置 > 系统 > 高级系统设置 > 环境变量 > 新建(系统变量)

#### macOS

1. CMake 由 Homebrew 安装后已在 PATH 中, 无需额外配置
2. 在 shell 配置文件(默认 `~/.zshrc`)中导出:
    - 变量名:VENUS_QT_DIR
    - 变量值:所下载的 Qt 中的 macos 文件夹, 例如 $HOME/Qt/6.8.3/macos

```bash
export VENUS_QT_DIR="$HOME/Qt/6.8.3/macos"
```

**设置完成后请重启终端或 IDE**

### 5. 生成构建系统

CMake 预设位于仓库根目录,在仓库根目录执行:

#### Windows

```bash
cmake --preset windows-ninja-clang-dev   # Ninja + Clang-cl → build-nc/(编译快，断言严格)
cmake --preset windows-vs-dev            # Visual Studio 17 2022 → build-vs/(编译慢，调试全面)
```

#### macOS

```bash
cmake --preset macos-dev    # Ninja + Apple Clang → build-nc/(单一日常通道，编译与调试同源)
```

### 6. 编译与测试

#### Windows

两条日常通道:

1. **`build-nc` + `Release|Debug|RelWithDebInfo`(首选)**:

    ```bash
    cmake --build build-nc --config RelWithDebInfo [--target <harken_core|harken_editor|harken_hub>]
    ```

2. **`build-vs` + `Release|Debug|RelWithDebInfo`(次选)**:

    ```bash
    cmake --build build-vs --config Debug  [--target <harken_core|harken_editor|harken_hub>]
    ```

#### macOS

单一日常通道: 

- **`build-nc` + `Release|Debug|RelWithDebInfo`**:

    ```bash
    cmake --build build-nc --config RelWithDebInfo [--target <harken_core|harken_editor|harken_hub>]
    ```

## 项目结构

```
harken/
├── .docs/                                # 项目全局文档、设计说明
├── .github/                              # GitHub 工程伴随工作流配置
├── .scripts/                             # 开发所需自动化脚本，非源代码
├── cmake/                                # 项目构建身份注入
├── HarkenEngine/                         # harken_core — 声音引擎运行核心
├── HarkenHub/                            # HarkenHub — 独立启动器程序
├── HarkenSchema/                         # Editor 与 Pack 各自独立的 JSON 合同
├── HarkenSDK/                            # HarkenSDK — 共享引擎 SDK 总目录（Unity/Harken）
├── HarkenStudio/                         # HarkenEditor — 前端编辑器
├── CLAUDE.md | AGENTS.md                 # AI 项目高层指令
├── CMake*                                # CMake 项目解决方案
├── requirements.txt                      # Python 依赖配置
├── OPEN_SOURCE_RELEASE_NOTICE*.txt       # 仓库开源说明
├── USER_TERMS*.md                        # 隐私政策和许可协议
└── README*.md | LICENSE | VERSION | NOTICE
```