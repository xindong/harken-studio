# VnResult

最近核对：2026-09-01。

权威源：`HarkenEngine/Source/Runtime/api/include/harken/Harken.h`。
数值钉扎：`HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json` `api.results.values`。
Studio 透传：`HarkenStudio/Source/RuntimeBridge/PlaybackBridge.h`（`EngineResult` 是 `uint32_t`；日志通常打印有符号值）。

`0` 是成功。负数分段：`-1xx` 参数，`-2xx` 身份，`-3xx` 容量/状态，`-4xx` 系统/设备/内容。非法输入不是未定义行为。

本表不是第二份 ABI。缺码时 grep `Harken.h` 和 wire table。

## 表

| 值 | 名字 | 含义 |
|---:|---|---|
| 0 | `VN_OK` | 成功 |
| -100 | `VN_INVALID_ARG` | 非法参数 |
| -101 | `VN_INVALID_CONFIG` | 非法配置 |
| -200 | `VN_UNKNOWN_ID` | 未知 id（对象/声音/总线/包/句柄不在） |
| -201 | `VN_STALE_HANDLE` | 句柄已过期 |
| -220 | `VN_CONTENT_NOT_LOADED` | catalog 命中，所属 Pack 未激活 |
| -221 | `VN_AUTHORITY_CONFLICT` | 同一 session 里 Pack 与 DirectWorld 互斥写入 |
| -222 | `VN_PACK_STALE_REVISION` | 内容修订与句柄配对不一致 |
| -300 | `VN_NO_VOICE` | voice 容量耗尽 |
| -301 | `VN_BACKPRESSURE` | 瞬时拒绝，零部分提交；`Vn_Update` 或资源短缺解除后可重试。Studio Preview 日志可能在同步等待超时后合成此码，见 [README.md](README.md) |
| -302 | `VN_IN_USE` | 资源占用中 |
| -303 | `VN_NOT_INITIALIZED` | Runtime 未初始化 |
| -304 | `VN_ALREADY_INITIALIZED` | Runtime 已初始化 |
| -305 | `VN_NO_BUS` | 总线表满或总线不存在（读返回它的调用） |
| -306 | `VN_NO_VARIABLE` | 变量不存在 / 变量容量 |
| -307 | `VN_NO_ATTENUATION` | 衰减配置不存在 / 容量 |
| -308 | `VN_NO_PACK` | Pack 容量耗尽 |
| -309 | `VN_NO_EFFECT` | 某效果类型的实例池耗尽 |
| -310 | `VN_NO_STREAM` | stream 槽表耗尽 |
| -311 | `VN_NO_ACTION` | Action firing 准入容量耗尽 |
| -312 | `VN_NO_CONTAINER` | 容器注册表或子项池满 |
| -313 | `VN_NO_EMITTER` | emitter 句柄表耗尽 |
| -314 | `VN_NO_SEND` | voice 或 bus 的 send 槽表满 |
| -315 | `VN_SEND_CYCLE` | bus send 边会构成环 |
| -316 | `VN_EMITTER_ATTACHED` | voice 跟随 emitter，拒绝逐 voice 空间写入 |
| -317 | `VN_TRIGGER_CYCLE` | Action trigger-step 图成环；零副作用拒绝 |
| -318 | `VN_TRACE_TOO_LATE` | `Vn_Init` 后已有状态 API，再 `Vn_TraceStart` |
| -319 | `VN_UNSUPPORTED_LAYOUT` | 本构建不能兑现的混音/设备/下混声道布局 |
| -320 | `VN_NO_BINDING` | binding 池耗尽；日志会写明哪一个 |
| -321 | `VN_METER_CYCLE` | 预留的控制图成环拒绝值 |
| -322 | `VN_NO_DYNAMIC_ROUTE` | dynamic route 不存在 / 容量 |
| -323 | `VN_DYNAMIC_ROUTE_CYCLE` | dynamic route 成环 |
| -324 | `VN_DYNAMIC_ROUTE_ORDER` | dynamic route 顺序拒绝 |
| -325 | `VN_REVISION_CONFLICT` | 修订冲突 |
| -326 | `VN_NO_TOPOLOGY_OWNER` | 拓扑所有者不存在 |
| -327 | `VN_CAPABILITY_UNAVAILABLE` | 当前 Runtime 没有该能力 |
| -328 | `VN_RUNTIME_RECREATE_REQUIRED` | 必须重建 Runtime |
| -329 | `VN_CURRENT_WORLD_CAPACITY` | Current World 容量超出 |
| -400 | `VN_DEVICE_ERROR` | 音频设备错误 |
| -401 | `VN_BAD_PACK` | 非法 Pack |
| -402 | `VN_BAD_STREAM_FILE` | `.stream` 不可读、损坏或校验失败 |
| -403 | `VN_BAD_TRACE_FILE` | trace 文件无法创建或打开写入 |
| -404 | `VN_CURRENT_WORLD_IO_ERROR` | DirectWorld 预览信封 I/O 错误 |
| -405 | `VN_BAD_CURRENT_WORLD` | 非法 DirectWorld 预览信封（`VNCWORLD`，不是独立 pack 合同） |
| -410 | `VN_PACK_INCOMPATIBLE` | Pack 与当前 Runtime 不兼容（含过期目录 vintage、非当前程序段 `bodyVintage`） |
| -411 | `VN_PACK_PATH_ESCAPE` | Pack 路径逃出允许根 |
| -412 | `VN_PACK_HASH_MISMATCH` | Pack 哈希不匹配 |
| -413 | `VN_PACK_CYCLE` | Pack 依赖成环 |
| -414 | `VN_PACK_CATALOG_CAPACITY` | Pack catalog 容量 |
| -415 | `VN_PACK_RESIDENT_CAPACITY` | Pack 常驻容量 |
| -416 | `VN_PACK_PROVIDER_FAILED` | Pack provider 失败 |
| -417 | `VN_PACK_INIT_LIFECYCLE` | Pack 初始化生命周期拒绝 |

`Harken.h` 未写注释的 `-32x` / `-41x` 只保留名字级含义。当作产品诊断前先读返回它的 API。

## 无符号打印

`EngineResult` 是 `uint32_t`。`-301` 无符号为 `4294966995`（`0xFFFFFED3`），`-200` 为 `4294967096`。Studio `auditionStartFailed` 当前按 `int32_t` 打日志，Log 面板看到的是有符号形式。

## 不是 VnResult 的相关枚举

- `VnCurrentWorldLoadStatus`：DirectWorld 预览直装报告状态，也从 `0` 起用小正整数。游戏加载 `.pack`，不把这份信封当成第三套磁盘合同。
- `VnPreviewResult`：见 [preview-result.md](preview-result.md)。
- miniaudio `ma_result`：解释 Harken 日志时忽略。
