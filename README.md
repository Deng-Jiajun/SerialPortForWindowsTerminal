# SerialPortForWindowsTerminal
Serial port for windows terminal, 让Windows Terminal支持串口的插件

该插件为WindowsTerminal提供访问串口终端能力


## 使用说明

![截图](https://github.com/Zhou-zhi-peng/SerialPortForWindowsTerminal/blob/main/images/001.bmp?raw=true)
![截图](https://github.com/Zhou-zhi-peng/SerialPortForWindowsTerminal/blob/main/images/002.bmp?raw=true)
![截图](https://github.com/Zhou-zhi-peng/SerialPortForWindowsTerminal/blob/main/images/003.bmp?raw=true)


## 配置说明：
![使用说明](https://github.com/Zhou-zhi-peng/SerialPortForWindowsTerminal/blob/main/images/000.gif?raw=true)

### 关键词高亮

「串口设置」中的「关键词高亮」用于标记串口接收内容。默认规则为 `ERROR` 使用红色、`WARN` 使用黄色。

- 每条规则包含一个关键词和一种预置颜色，可添加、更新或删除。
- 预置颜色包括红色、黄色、绿色、青色、蓝色和品红。
- 关键词按完整字面量匹配并区分大小写，不支持正则表达式。
- 高亮只影响 Windows Terminal 显示，不修改串口接收数据或用户发送内容。
- 关键词按当前选择的 UTF-8 或 GBK 编码匹配。

### 外部命令控制

串口连接成功后，主程序会启动仅限本机访问的命名管道 `\\.\pipe\SerialForWindowsTerminal`。Codex、脚本或测试程序可使用同目录下的 `SerialTerminalCtl.exe` 发送命令并读取响应：

```powershell
.\SerialTerminalCtl.exe exec "cd /customer"
.\SerialTerminalCtl.exe exec --prompt "# " "ls -l"
```

外部输入会以 `[External] > 命令` 的形式显示在串口终端中，设备响应仍实时显示并应用关键词高亮；控制程序同时从标准输出返回未高亮的 UTF-8 文本。外部命令执行期间，键盘输入不会写入串口，避免两路命令交叉。

控制参数：

- `--prompt <文本>`：响应以指定提示符结尾时完成。
- `--idle <毫秒>`：收到响应后持续无新数据即完成，默认 `300`。
- `--timeout <毫秒>`：命令总超时，默认 `5000`。
- `--line-ending <类型>`：发送命令使用 `none`、`cr`、`lf` 或 `crlf`，默认 `crlf`。

提示符匹配和空闲时间任一条件满足即视为成功；总超时返回退出码 `2`，其他错误返回退出码 `3`。同一时间只处理一个外部连接。若启动了多个串口终端实例，只有成功创建固定命名管道的实例可接受外部命令。
