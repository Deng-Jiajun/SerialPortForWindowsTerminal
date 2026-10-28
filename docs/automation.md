# 外部命令控制

[返回 README](../README.md)

串口连接成功后，主程序会启动仅限本机访问的命名管道 `\\.\pipe\SerialForWindowsTerminal`。Codex、脚本或测试程序可使用同目录下的 `SerialTerminalCtl.exe` 发送命令并读取响应：

控制程序通过命名管道向已连接串口的主程序提交命令，不会再次打开 COM 口；无需手工创建管道。以下命令在解压目录执行。

```powershell
.\SerialTerminalCtl.exe exec --line-ending cr "cd /customer"
.\SerialTerminalCtl.exe exec --line-ending cr --prompt "# " "ls -l"
```

外部输入会以 `[TX External] > 命令` 的形式显示在串口终端中，设备响应仍实时显示并应用关键词高亮；控制程序同时从标准输出返回未高亮、未加入时间戳的 UTF-8 文本。外部命令执行期间，普通键盘输入不会写入串口，本地草稿保留。

控制参数：

- `--prompt <文本>`：响应以指定提示符结尾时完成
- `--idle <毫秒>`：收到响应后持续无新数据即完成，默认 `300`
- `--timeout <毫秒>`：命令总超时，默认 `5000`
- `--line-ending <类型>`：发送命令使用 `none`、`cr`、`lf` 或 `crlf`，默认 `crlf`；连接 Linux shell 时显式使用 `--line-ending cr`，避免多提交一次空行

提示符匹配和空闲时间任一条件满足即视为成功；总超时返回退出码 `2`，通信等错误返回退出码 `3`，参数不正确时显示用法并返回 `64`（当前 `--help` 也按此方式处理）。同一时间只处理一个外部连接。若启动了多个串口终端实例，只有成功创建固定命名管道的实例可接受外部命令。
