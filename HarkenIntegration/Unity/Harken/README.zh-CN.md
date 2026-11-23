# Harken for Unity

[English](README.md) | **简体中文**

`com.harken.core` 把 Harken 游戏音频引擎集成到 Unity 中。

文档：ONLINE_DOCUMENTATION_URL_PENDING

## 产品定位

Harken 是一套游戏音频中间件。声音设计师在 Harken Studio 中编排声音与播放逻辑，并导出为 Pack；本包在 Unity 游戏中加载这些 Pack，并通过 Harken 引擎播放，游戏代码按名称触发声音，而不必直接处理音频文件。

## 在 Windows 游戏启动时准备首关声音

如果游戏自行管理音频，可以在 Unity 主线程调用 `await PackBootstrap.StartLevelAsync(source, config, actionNames, cancellation)`。传入平台 Pack 目录创建的 `PackSource` 和首关需要的 Action 名称。支持后台打开设备的 Windows Runtime 会同时准备这些声音。游戏需要立即播放开场声音时，可以在 `onPrepared` 回调中创建发声体、提交 Action，并返回提交结果。该声音会在真实设备上从头播放。返回 `VN_OK` 后设备和首关声音都已就绪。退出关卡时调用 `Preparation.Release()`，结束音频会话时调用 `Session.Shutdown()`。使用默认音频组件的场景仍由组件负责启动和关闭。

## 功能特性

- 在项目设置中选择 Pack 目录即可启动引擎，也可以在代码中以目录、字节或自定义来源启动。
- 按名称投递 Action、停止 Action，并跟踪其播放状态。
- 按名称写入变量，驱动设计好的参数。
- 发声体与听者组件跟随 Unity Transform，实现 3D 空间定位。
- 显式加载和释放 Pack，支持按语言、按平台的 Pack 目录。
- 在 Unity 编辑器中预览 Action。
- 让 Harken Studio 连接正在运行的游戏进行实时观察。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| Unity | 2022.3 及以上 |
| 平台 | Windows x64、macOS、Linux x64、Android arm64-v8a、iOS |
| Unity 内置模块 | Audio、JSONSerialize、UnityWebRequest（已声明为包依赖） |
| 内容 | 由 Harken Studio 导出的 Pack |

## 版本与兼容

- 包版本即 `package.json` 中的 `version` 字段，与 Harken Studio、Harken 引擎的版本相互独立。
- 包内带有两个原生库版本：正式版（`HarkenEngine`）与开发版（`HarkenEngineDev`）。在 **Project Settings > Harken > Project > Native Library** 中选择：*Automatic* 在编辑器与 Development Build 中使用开发版，其余情况使用正式版；*Release* 与 *Development* 在所有情况下固定使用对应版本。修改后需重启 Unity。
- 只存在于开发版的 API 仅在定义 `HARKEN_ENGINE_DEV` 时编译，正式构建不会引用它们。
- 原生库的 ABI 主版本与托管绑定不一致时，拒绝启动。
- 每个包版本读取一个 Pack 元数据版本（当前为 4），请使用写出该版本的 Harken Studio 导出 Pack。Pack 二进制版本由引擎自行校验。

## 许可

见 [LICENSE.md](LICENSE.md) 与 [NOTICE.md](NOTICE.md)。

### 减小 Unity 发行目录

使用 Python 3.9 或更高版本运行 SDK 的发布命令，指定已安装的 Studio CLI、保存的工程、目标平台 UUID 和一个新的发行目录。

```sh
python Tools~/publish_packs.py --studio-cli /path/to/HarkenStudioCLI --project GameAudio.hsp --platform-id <uuid> --output release-042
```

将输出的平台目录交给现有 Unity Pack 启动、目录或 reader 入口即可。工具先用 Studio 正式导出，再复制完整发布物。每个不超过 16 MiB 且压缩后更小的 Pack 独立存为 gzip，原文件名和相对路径保留。更大的 Pack、压缩后不节省空间的 Pack、配套 JSON 及其他辅助文件保留原字节。目录加载在准备阶段从文件直接解压到一个有上限的数组，reader 加载在此期间还持有输入数组。SDK 将还原的原始 Pack 交给既有校验和加载路径。

直接使用原生文件 API 或旧版 Unity SDK 的项目可使用 Studio CLI 的常规导出目录。如果交付渠道已有外层压缩，请按最终归档大小评估收益。

在 Windows 上为 Windows 游戏发布常驻音效时，可在发布命令末尾添加 `--format windows-resident`。把生成的整个平台目录随游戏分发，`PackBootstrap.StartDirectory` 会识别 `Init.hrp`。进入关卡时用 `Session.PrepareActions` 一次传入本关的 Action 名称，成功返回后即可按名播放。每个 Action 有独立的 `.hrp` 文件。使用同一项目和兼容的 Init 更新单个声音时，只需分发变化的文件。

需要标准 ZIP 时，在同一命令末尾添加 `--format zip`。工具会生成 `<输出目录>/<平台>/Packs.zip`，归档根目录直接包含 Studio 导出的 Pack、配套 JSON 和辅助文件，保留各自的相对路径和原始字节。16 MiB 以内且压缩后更小的条目使用 DEFLATE，其余条目原样存储。使用标准 ZIP 工具解包后，普通文件加载入口可直接使用该平台目录。

操作返回 `VN_RUNTIME_RECREATE_REQUIRED` 时，可以通过 `HarkenAudioSystem.RuntimeRecreationRequired` 事件或 `HarkenEngine.QueryRuntimeFault(out RuntimeFaultInfo info)` 查询首次故障。停止提交新操作，关闭当前会话后重新初始化。单次操作返回 `VN_OUT_OF_MEMORY` 时，当前会话仍可继续使用。
