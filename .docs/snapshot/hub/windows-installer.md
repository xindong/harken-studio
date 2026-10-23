# Windows Hub 安装包构建

`.scripts/release/build_hub_installer.py` 调用仓库内的 `hub_msi.py` 和 `windows_installer.py`，使用 Windows 自带的 Windows Installer API 与 `makecab.exe` 生成 MSI，再嵌入现有 `HarkenStudioHubSetup.exe`。构建不再安装 WiX 或其 UI 扩展。系统工具不复制进分发物；项目许可证不变。

## 构建与安装合同

- `.github/workflows/hub-artifact.yml` 保留原有产物命名、artifact 名称和证据目录。需要 Windows、Python 和项目原有构建环境。
- MSI 使用 x64、按计算机安装、Windows Installer 5.0；UpgradeCode 保持 `4EA0003D-209E-45E6-8343-67529D7E7E5A`。每个新包使用新的 ProductCode 与 PackageCode。
- 文件组件继续按原命名空间和相对路径生成 GUID；原目录 ACL、关联和快捷方式组件的 GUID、键路径与位数保持不变。不要为日常版本升级重新生成组件身份。
- 同版本包允许替换，较旧版本拒绝安装；旧产品在新包提交后移除。HKLM 的 `Software\HarkenSound\HarkenHub\InstallDir` 恢复既有安装路径。
- 安装界面由项目自行定义，展示原法务材料生成的 RTF、接受选项和安装路径；维护安装支持修复及卸载。`/qn`、`/qb!` 等静默模式继续由 Windows Installer 处理。
- 自更新继续使用 `HARKENSELFUPDATE` / `HARKENBRIDGE`，保留注册与注销更新桥、健康握手、重启动作及其执行权限、条件和顺序。
- `packaging-inputs/msi-tables.json` 保留本次实际写入的 MSI 数据；证据同时关联生成器源码和 `license.rtf`。

## 数字签名与发布

构建端配置 `HARKEN_AUTHENTICODE_THUMBPRINT` 后，依次执行：签 MSI → 将已签 MSI 嵌入 EXE 的 `HARKENHUBMSI` 资源 → 签 EXE → 计算产物哈希。`HARKEN_AUTHENTICODE_TIMESTAMP_URL` 继续传给 `signtool /tr`，摘要算法保持 SHA-256。签名失败不得输出可发布的半成品。

`audio-harken-managing-tool` 仓库的 `publishing/core.py::authenticode_run` 对最终 EXE 调用签名服务、验证签名，然后更新大小与 SHA-256，并保留 `unsignedSha256` 关联构建证据；之后才签发布清单。它没有拆出、重新签署内嵌 MSI。本次保持这些文件名、资源名和证据字段，发布工具无需修改。

## 验证

`python .scripts/tests/test_build_hub_installer.py -v` 检查包内容准入、签名顺序、最终哈希及失败原子性。在 Windows 上还实际生成 MSI，并通过 Windows Installer 管理解包逐文件比对；读取最终数据库核对升级身份、目录权限、桥接动作及法务原文。

迁移验收使用已发布的 0.1.67 MSI 与新生成器产物，在独立产品 GUID、测试注册表键、用户目录下完成旧包安装、新包升级、同版本替换、降级拒绝、修复及卸载，保留自定义路径和额外用户文件。该隔离验证不执行真实更新桥注册。真实按计算机安装、提权与完整交互界面仍应在干净 Windows 环境验收。

本地签名验证使用临时自签测试证书，确认 MSI/EXE 均可签署、外层签名后内嵌 MSI 字节不变、篡改会触发摘要不匹配。测试证书未加入系统信任库，不能替代正式证书、时间戳服务与发布平台的最终验证。
