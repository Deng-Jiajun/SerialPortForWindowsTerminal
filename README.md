# SerialPortForWindowsTerminal

在 Windows Terminal 中连接串口，支持 UTF-8 / GBK、关键词高亮、日志时间戳和本地命令历史。

[下载最新版](https://github.com/Deng-Jiajun/SerialPortForWindowsTerminal/releases/latest) · [v1.1.1 更新说明](docs/release-notes/v1.1.1.md)

## 下载与连接

1. 从 Releases 下载对应架构的 ZIP 并解压，64 位 Windows 通常选择 `x64`
2. 打开 Windows Terminal「设置」，添加一个新的空配置文件
3. 将「命令行」设为解压后的主程序路径，例如 `"D:\Tools\SerialPortForWindowsTerminal\SerialForWindowsTerminal.exe"`，保存
4. 打开该配置，在「串口设置」中选择串口号、波特率、编码和输入模式，确认后连接

`SerialForWindowsTerminal.exe` 是串口终端；同目录的 `SerialTerminalCtl.exe` 供脚本发送命令，手动使用时无需启动。

![串口设置](images/settings.png)

截图为示例配置，串口号和关键词高亮规则以本机设置为准。

## 日常使用

**本地编辑（历史建议）**适合输入命令、查看日志。输入行固定在底部，按回车后整行发送，上方日志持续滚动；输入前缀匹配历史命令时，剩余内容以灰色显示。

**字符透传**适合登录密码、`vim`、`top` 等直接交互。使用这些功能时关闭时间戳；需要向设备发送 `Ctrl+D` EOF 时，也先切换到此模式。

| 按键 | 操作 |
| --- | --- |
| `Ctrl+K` | 打开「终端设置」，两种输入模式均可使用 |
| `Ctrl+C` | 本地编辑时取消草稿，立即发送中断字节 `0x03`，显示青色 `> ^C` 记录，无需回车 |
| `Enter` | 本地编辑时发送当前命令，末尾只追加一个 `CR`（`\r`） |
| `→` | 本地编辑时，光标在行末则接受灰色建议，否则向右移动 |
| `Tab` | 接受历史建议，不查询设备文件或补全设备命令 |
| `↑` / `↓` | 按当前前缀翻阅历史，回到最新位置时恢复草稿 |
| `←` / `→`、`Home` / `End` | 移动本地输入光标 |
| `Backspace` / `Delete` | 删除光标前 / 后的字符 |

「终端设置」可修改编码、输入模式和时间戳，也可退出终端。弹窗打开期间继续接收日志并保留草稿；「确定」应用并保存，「取消」或关闭弹窗不修改设置。串口号、波特率和关键词高亮仍在连接前配置。

本地编辑不处理 `Ctrl+F`、`Ctrl+U`、`Ctrl+W`、`Ctrl+D`；字符透传中除 `Ctrl+K` 外，其余按键原样发送。

## 日志显示

- 时间戳默认开启，格式为 `[HH:mm:ss.fff]`，使用电脑本地时间
- 本地命令显示为青色的 `[HH:mm:ss.fff] > 命令`；发送后 1 秒内，首条接收行与命令完全相同时，隐藏这一条设备回显，避免重复显示
- 空输入按回车只发送一个 `CR`，显示一条 `>` 发送记录；设备返回的空行和新提示符仍会显示
- 关键词按完整字面量匹配并区分大小写，不支持正则表达式；默认 `ERROR` 为红色、`WARN` 为黄色
- 可为每条关键词选择红、黄、绿、青、蓝、品红，并添加、更新或删除规则

串口没有独立的回显标记：设备关闭回显后，若首条实际输出恰好与命令相同，也会被隐藏。字符透传和外部控制命令不启用该过滤。时间戳、颜色和回显过滤只影响显示，不改写串口数据。

更多说明见[输入、历史与日志](docs/terminal.md)。

## 串口断开

串口接收失败时，终端显示错误码、原因及重新打开的提示，并以非零退出码结束。Windows Terminal 配置为 `closeOnExit: automatic` 时会保留错误页；若设为 `always`，标签页仍会关闭。

程序不自动重连。重新插入设备后，重新打开该配置连接。

## 文档

- [输入、历史与日志](docs/terminal.md)：草稿、编码切换、历史保存、时间戳和回显过滤规则
- [外部命令控制](docs/automation.md)：通过 `SerialTerminalCtl.exe` 发送命令并读取响应
- [构建、测试与发布](docs/development.md)：开发环境、验证命令和发布步骤
- [v1.1.1 更新说明](docs/release-notes/v1.1.1.md)
- [v1.1.0 更新说明](docs/release-notes/v1.1.0.md)

本项目基于 [Zhou-zhi-peng/SerialPortForWindowsTerminal](https://github.com/Zhou-zhi-peng/SerialPortForWindowsTerminal)，使用 [Apache-2.0 许可证](LICENSE)。
