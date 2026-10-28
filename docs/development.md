# 构建、测试与发布

[返回 README](../README.md)

## 构建与测试

需要 Visual Studio C++ 工具集 `v142`、Windows SDK，以及上级目录的 `boost_1_68_0`。可在 Visual Studio 2019 或安装了 `v142` 的 Visual Studio 2022 中构建；Boost 可通过 `/p:BoostRoot="D:\Libraries\boost_1_68_0"` 指定。以下命令在开发人员 PowerShell 的仓库根目录执行。

```powershell
msbuild SerialForWindowsTerminal.sln /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
msbuild tests\TerminalInteractionTests.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
$testDirectory = Join-Path $PWD ('artifacts\tests\run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
.\artifacts\tests\x64\TerminalInteractionTests.exe (Join-Path $testDirectory 'history.txt')
```

自动检查覆盖串口分包、换行、UTF-8/GBK、ANSI 颜色、关键词高亮、历史恢复与并发保存、灰色建议、草稿恢复、中文编辑及粘贴。回车和 `Ctrl+K` 检查以字节管道代替串口，经过实际输入处理和发送队列，确认命令与空行均只发送一个 `CR`，设置快捷键不发送给设备，已移除的本地快捷键不改变草稿。设置检查向实际弹窗发送控件消息，验证确定、取消、关闭、退出、草稿编码拒绝及弹窗打开期间的日志输出。断开检查覆盖错误提示、非零错误状态、主动取消与排队写入取消。

测试使用独立历史文件和本进程临时注册表，不访问实际设备。测试程序的 `--preview` 参数显示持续模拟日志和输入建议；`--runtime-settings` 同时打开「终端设置」，供 GUI 验证使用。

构建 x86 时，解决方案使用 `/p:Platform=x86`，测试项目使用 `/p:Platform=Win32`，测试程序位于 `artifacts\tests\Win32`。x64 的两个 EXE 输出到 `x64\Release`，x86 输出到 `Release`。

## 关闭处理

点击标签页关闭按钮时，程序接管控制台关闭通知，取消串口和输入输出操作，停止外部控制线程后退出。清理等待最多 3 秒，超时后交由系统结束进程。

控制台关闭处理在串口设置对话框结束后重新注册，避免输入法组件卸载后留下的控制台回调被继续调用。断开错误按终端当前的 UTF-8 / GBK 编码显示。系统关机属于另外的现场验证场景，自动测试不等同于关机验证。

## 发布

GitHub Actions 在 `main` 推送及 Pull Request 时构建 Debug / Release、x86 / x64 四种组合。推送 `v*` 标签会构建 x86、x64 Release 并发布到 GitHub Releases。

发布新版本时：

1. 更新主程序启动版本、资源文件的文件版本 / 产品版本及「关于」文本
2. 更新 README，并在 `docs/release-notes/<标签>.md` 编写对应版本说明
3. 完成两个 Release 架构的 Rebuild 和测试，提交变更后推送 `main`
4. 为该提交创建版本标签并推送，例如：

```powershell
git tag -a v1.1.0 -m "SerialPortForWindowsTerminal v1.1.0"
git push origin v1.1.0
```

发布流程使用对应标签的说明文件创建 Release。每个 ZIP 包含主程序、控制程序、README、`docs`、设置截图和许可证；`SHA256SUMS.txt` 提供两个 ZIP 的 SHA-256。发布后检查 Actions 结果及 Release 附件。

构建产物不提交到源码仓库，下载版本统一放在 GitHub Releases。
