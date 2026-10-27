// SerialForWindowsTerminal.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "SerialForWindowsTerminal.h"
#include "SerialAutomationProtocol.h"
#include "TerminalInteraction.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <boost/asio.hpp> 
#include <boost/asio/windows/object_handle.hpp>
#include <boost/asio/windows/stream_handle.hpp>

#define MAX_LOADSTRING 100

// 全局变量:
HINSTANCE hInstance;
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    SettingFunc(HWND, UINT, WPARAM, LPARAM);

// 添加全局标志来跟踪Ctrl+A状态
bool g_isCtrlAPressed = false;
// 添加全局标志来控制程序退出
bool g_shouldExit = false;

// 控制台回调在系统线程运行；句柄保留到进程结束，避免退出时与回调争用析构资源
static HANDLE g_consoleCloseRequested = nullptr;
static HANDLE g_consoleMainThread = nullptr;

static BOOL WINAPI ConsoleControlHandler(DWORD event)
{
    if (event != CTRL_CLOSE_EVENT && event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT)
        return FALSE;

    SetEvent(g_consoleCloseRequested);
    // 主线程正常退出会结束整个进程；清理超时后返回 TRUE，仍不再调用后续失效回调
    WaitForSingleObject(g_consoleMainThread, 3000);
    return TRUE;
}

static bool RegisterConsoleControlHandler()
{
    // 设置对话框可能加载输入法并注册回调，关闭对话框后重新排到处理列表首位
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    return SetConsoleCtrlHandler(ConsoleControlHandler, TRUE) != FALSE;
}

static std::string ErrorMessage(const boost::system::error_code& error, UINT codePage)
{
    auto result = std::to_string(error.value());
    if (error.category() != boost::system::system_category())
        return result + ": " + error.message();

    // Boost 1.68 的系统错误文本使用系统代码页，不能直接写到 UTF-8 终端
    wchar_t message[1024] = {};
    auto size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                              error.value(), 0, message, _countof(message), nullptr);
    while (size && (message[size - 1] == L'\r' || message[size - 1] == L'\n'))
        --size;
    auto bytes = EncodeTerminalText(std::wstring(message, size), codePage);
    if (!bytes.empty())
        result += ": " + std::string(bytes.begin(), bytes.end());
    return result;
}

enum HIGHLIGHT_COLOR : DWORD
{
    HIGHLIGHT_COLOR_RED = 0,
    HIGHLIGHT_COLOR_YELLOW,
    HIGHLIGHT_COLOR_GREEN,
    HIGHLIGHT_COLOR_CYAN,
    HIGHLIGHT_COLOR_BLUE,
    HIGHLIGHT_COLOR_MAGENTA,
    HIGHLIGHT_COLOR_COUNT
};

typedef struct
{
    std::wstring Keyword;
    DWORD Color;
}HIGHLIGHT_RULE;

static const wchar_t* GetHighlightColorName(DWORD color)
{
    switch (color)
    {
    case HIGHLIGHT_COLOR_RED:
        return L"红色";
    case HIGHLIGHT_COLOR_YELLOW:
        return L"黄色";
    case HIGHLIGHT_COLOR_GREEN:
        return L"绿色";
    case HIGHLIGHT_COLOR_CYAN:
        return L"青色";
    case HIGHLIGHT_COLOR_BLUE:
        return L"蓝色";
    case HIGHLIGHT_COLOR_MAGENTA:
        return L"品红";
    default:
        return L"红色";
    }
}

static const char* GetHighlightColorSequence(DWORD color)
{
    switch (color)
    {
    case HIGHLIGHT_COLOR_RED:
        return "\033[91m";
    case HIGHLIGHT_COLOR_YELLOW:
        return "\033[93m";
    case HIGHLIGHT_COLOR_GREEN:
        return "\033[92m";
    case HIGHLIGHT_COLOR_CYAN:
        return "\033[96m";
    case HIGHLIGHT_COLOR_BLUE:
        return "\033[94m";
    case HIGHLIGHT_COLOR_MAGENTA:
        return "\033[95m";
    default:
        return "\033[91m";
    }
}

using PortsArray = std::vector<std::pair<std::wstring, int>>;

static PortsArray GetAllPorts(void)
{
    PortsArray ports;
    HKEY hRegKey;
    int nCount = 0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Hardware\\DeviceMap\\SerialComm", 0, KEY_READ, &hRegKey) == ERROR_SUCCESS)
    {
        while (true)
        {
            TCHAR szName[MAX_PATH] = { 0 };
            TCHAR szPort[MAX_PATH] = { 0 };
            DWORD nValueSize = MAX_PATH - 1;
            DWORD nDataSize = MAX_PATH - 1;
            DWORD nType;

            if (::RegEnumValue(hRegKey, nCount, szName, &nValueSize, NULL, &nType, (LPBYTE)szPort, &nDataSize) == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            std::wstring name(szName);
            auto idx = name.find_last_of('\\');
            if (idx != name.npos)
            {
                name = name.substr(idx + 1);
            }
            name += L" (";
            name += szPort;
            name += L")";
            ports.push_back(std::make_pair(name, (int)std::wcstoul(szPort + 3, nullptr, 10)));
            nCount++;
        }
        ::RegCloseKey(hRegKey);
    }
    return ports;
}

static void UpdatePortControl(HWND hDlg)
{
    auto allPorts = GetAllPorts();
    auto hWndPort = GetDlgItem(hDlg, IDC_COMBO_PORT);
    ComboBox_ResetContent(hWndPort);
    for (auto port : allPorts)
    {
        auto index = ComboBox_AddString(hWndPort, port.first.c_str());
        ComboBox_SetItemData(hWndPort, index, port.second);
    }
}

static void CenterParentWindow(HWND hWnd)
{
    RECT rcDlg;
    ::GetWindowRect(hWnd, &rcDlg);
    RECT rcParent;
    HWND hWndParent = GetParent(hWnd);
    GetClientRect(hWndParent, &rcParent);
    POINT ptParentInScreen;
    ptParentInScreen.x = rcParent.left;
    ptParentInScreen.y = rcParent.top;
    ::ClientToScreen(hWndParent, (LPPOINT)&ptParentInScreen);
    SetWindowPos(
        hWnd,
        NULL,
        ptParentInScreen.x + (rcParent.right - rcParent.left - (rcDlg.right - rcDlg.left)) / 2,
        ptParentInScreen.y + (rcParent.bottom - rcParent.top - (rcDlg.bottom - rcDlg.top)) / 2,
        0,
        0,
        SWP_NOZORDER | SWP_NOSIZE);
}

typedef struct
{
    DWORD Serial;
    DWORD BaudRate;
    DWORD WordLength;
    DWORD StopBit;
    DWORD Parity;
    DWORD FlowControl;
    DWORD EncodingFormat;  // 添加编码格式字段：0=UTF8, 1=GBK
    DWORD EchoMode;        // 添加回显模式字段：0=关闭(字符模式), 1=开启(行模式)
    DWORD Timestamps;
    std::vector<HIGHLIGHT_RULE> HighlightRules;
}SERIAL_CONFIG;

static void WriteHighlightRules(HKEY hKey, const std::vector<HIGHLIGHT_RULE>& rules)
{
    std::vector<wchar_t> data;
    for (const auto& rule : rules)
    {
        if (rule.Keyword.empty() || rule.Color >= HIGHLIGHT_COLOR_COUNT)
        {
            continue;
        }

        auto value = std::to_wstring(rule.Color) + L"\t" + rule.Keyword;
        data.insert(data.end(), value.begin(), value.end());
        data.push_back(L'\0');
    }

    if (data.empty())
    {
        data.push_back(L'\0');
    }
    data.push_back(L'\0');

    ::RegSetValueEx(
        hKey,
        L"HighlightRules",
        0,
        REG_MULTI_SZ,
        reinterpret_cast<const BYTE*>(data.data()),
        static_cast<DWORD>(data.size() * sizeof(wchar_t)));
}

static bool ReadHighlightRules(HKEY hKey, std::vector<HIGHLIGHT_RULE>& rules)
{
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueEx(hKey, L"HighlightRules", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_MULTI_SZ || size < sizeof(wchar_t))
    {
        return false;
    }

    std::vector<wchar_t> data(size / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueEx(
            hKey,
            L"HighlightRules",
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(data.data()),
            &size) != ERROR_SUCCESS)
    {
        return false;
    }

    std::vector<HIGHLIGHT_RULE> loadedRules;
    for (const wchar_t* value = data.data(); *value != L'\0'; value += std::wcslen(value) + 1)
    {
        std::wstring entry(value);
        auto separator = entry.find(L'\t');
        if (separator == std::wstring::npos || separator + 1 >= entry.size())
        {
            continue;
        }

        auto color = std::wcstoul(entry.substr(0, separator).c_str(), nullptr, 10);
        if (color >= HIGHLIGHT_COLOR_COUNT)
        {
            continue;
        }

        loadedRules.push_back({entry.substr(separator + 1), color});
    }

    rules = std::move(loadedRules);
    return true;
}

static void WriteSerialConfig(const SERIAL_CONFIG& cfg)
{
    HKEY hKey;
    if (ERROR_SUCCESS == ::RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\SerialForWindowsTerminal", &hKey))
    {
        DWORD dwSize = sizeof(DWORD);
        DWORD dwType = REG_DWORD;

        ::RegSetValueEx(hKey, L"Serial", 0, dwType, (CONST LPBYTE)&cfg.Serial, dwSize);
        ::RegSetValueEx(hKey, L"BaudRate", 0, dwType, (CONST LPBYTE)&cfg.BaudRate, dwSize);
        ::RegSetValueEx(hKey, L"WordLength", 0, dwType, (CONST LPBYTE)&cfg.WordLength, dwSize);
        ::RegSetValueEx(hKey, L"StopBit", 0, dwType, (CONST LPBYTE)&cfg.StopBit, dwSize);
        ::RegSetValueEx(hKey, L"Parity", 0, dwType, (CONST LPBYTE)&cfg.Parity, dwSize);
        ::RegSetValueEx(hKey, L"FlowControl", 0, dwType, (CONST LPBYTE)&cfg.FlowControl, dwSize);
        ::RegSetValueEx(hKey, L"EncodingFormat", 0, dwType, (CONST LPBYTE)&cfg.EncodingFormat, dwSize);
        ::RegSetValueEx(hKey, L"EchoMode", 0, dwType, (CONST LPBYTE)&cfg.EchoMode, dwSize);  // 写入回显模式
        ::RegSetValueEx(hKey, L"Timestamps", 0, dwType, (CONST LPBYTE)&cfg.Timestamps, dwSize);
        WriteHighlightRules(hKey, cfg.HighlightRules);
        ::RegCloseKey(hKey);
    }
}

static SERIAL_CONFIG ReadSerialConfig()
{
    HKEY hKey;
    SERIAL_CONFIG cfg;
    cfg.Serial = 0;
    cfg.BaudRate = 9600;
    cfg.WordLength = 8;
    cfg.StopBit = ONESTOPBIT;
    cfg.Parity = NOPARITY;
    cfg.FlowControl = 0;
    cfg.EncodingFormat = 0;  // 默认使用UTF-8编码
    cfg.EchoMode = 0;        // 默认关闭回显模式（字符模式）
    cfg.Timestamps = 1;
    cfg.HighlightRules.push_back({L"ERROR", HIGHLIGHT_COLOR_RED});
    cfg.HighlightRules.push_back({L"WARN", HIGHLIGHT_COLOR_YELLOW});
    if (ERROR_SUCCESS == ::RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\SerialForWindowsTerminal", 0, KEY_READ, &hKey))
    {
        DWORD dwSize = sizeof(DWORD);
        DWORD dwType = REG_DWORD;

        ::RegQueryValueEx(hKey, L"Serial", 0, &dwType, (LPBYTE)&cfg.Serial, &dwSize);
        ::RegQueryValueEx(hKey, L"BaudRate", 0, &dwType, (LPBYTE)&cfg.BaudRate, &dwSize);
        ::RegQueryValueEx(hKey, L"WordLength", 0, &dwType, (LPBYTE)&cfg.WordLength, &dwSize);
        ::RegQueryValueEx(hKey, L"StopBit", 0, &dwType, (LPBYTE)&cfg.StopBit, &dwSize);
        ::RegQueryValueEx(hKey, L"Parity", 0, &dwType, (LPBYTE)&cfg.Parity, &dwSize);
        ::RegQueryValueEx(hKey, L"FlowControl", 0, &dwType, (LPBYTE)&cfg.FlowControl, &dwSize);
        ::RegQueryValueEx(hKey, L"EncodingFormat", 0, &dwType, (LPBYTE)&cfg.EncodingFormat, &dwSize);
        ::RegQueryValueEx(hKey, L"EchoMode", 0, &dwType, (LPBYTE)&cfg.EchoMode, &dwSize);  // 读取回显模式
        ::RegQueryValueEx(hKey, L"Timestamps", 0, &dwType, (LPBYTE)&cfg.Timestamps, &dwSize);
        ReadHighlightRules(hKey, cfg.HighlightRules);
        ::RegCloseKey(hKey);
    }
    return cfg;
}

static boost::system::error_code InitializeSerialPort(boost::asio::serial_port& serialPort,const SERIAL_CONFIG& cfg, boost::system::error_code& ec)
{
    serialPort.set_option(boost::asio::serial_port::baud_rate(cfg.BaudRate), ec);
    if (ec)
        return ec;
    serialPort.set_option(boost::asio::serial_port::character_size(cfg.WordLength), ec);
    if (ec)
        return ec;
    switch (cfg.StopBit)
    {
    case 0:
        serialPort.set_option(boost::asio::serial_port::stop_bits(boost::asio::serial_port::stop_bits::one), ec);
        break;
    case 1:
        serialPort.set_option(boost::asio::serial_port::stop_bits(boost::asio::serial_port::stop_bits::onepointfive), ec);
        break;
    case 2:
        serialPort.set_option(boost::asio::serial_port::stop_bits(boost::asio::serial_port::stop_bits::two), ec);
        break;
    default:
        serialPort.set_option(boost::asio::serial_port::stop_bits(boost::asio::serial_port::stop_bits::one), ec);
        break;
    }
    if (ec)
        return ec;

    switch (cfg.Parity)
    {
    case 0:
        serialPort.set_option(boost::asio::serial_port::parity(boost::asio::serial_port::parity::none), ec);
        break;
    case 1:
        serialPort.set_option(boost::asio::serial_port::parity(boost::asio::serial_port::parity::odd), ec);
        break;
    case 2:
        serialPort.set_option(boost::asio::serial_port::parity(boost::asio::serial_port::parity::even), ec);
        break;
    default:
        serialPort.set_option(boost::asio::serial_port::parity(boost::asio::serial_port::parity::none), ec);
        break;
    }
    if (ec)
        return ec;

    switch (cfg.FlowControl)
    {
    case 0:
        serialPort.set_option(boost::asio::serial_port::flow_control(boost::asio::serial_port::flow_control::none), ec);
        break;
    case 1:
        serialPort.set_option(boost::asio::serial_port::flow_control(boost::asio::serial_port::flow_control::software), ec);
        break;
    case 2:
        serialPort.set_option(boost::asio::serial_port::flow_control(boost::asio::serial_port::flow_control::hardware), ec);
        break;
    default:
        serialPort.set_option(boost::asio::serial_port::flow_control(boost::asio::serial_port::flow_control::none), ec);
        break;
    }
    
    return ec;
}

typedef struct
{
    std::vector<uint8_t> Keyword;
    DWORD Color;
}ENCODED_HIGHLIGHT_RULE;

typedef struct
{
    std::vector<ENCODED_HIGHLIGHT_RULE> Rules;
    std::vector<uint8_t> Pending;
    DWORD EncodingFormat;
}HIGHLIGHT_OUTPUT_STATE;

static std::vector<uint8_t> EncodeHighlightKeyword(const std::wstring& keyword, UINT codePage)
{
    if (keyword.empty())
    {
        return {};
    }

    BOOL usedDefaultChar = FALSE;
    auto requiredSize = codePage == CP_UTF8
        ? ::WideCharToMultiByte(codePage, 0, keyword.data(), static_cast<int>(keyword.size()), nullptr, 0, nullptr, nullptr)
        : ::WideCharToMultiByte(codePage, 0, keyword.data(), static_cast<int>(keyword.size()), nullptr, 0, "?", &usedDefaultChar);
    if (requiredSize <= 0 || usedDefaultChar)
    {
        return {};
    }

    std::vector<uint8_t> encoded(static_cast<size_t>(requiredSize));
    usedDefaultChar = FALSE;
    auto convertedSize = codePage == CP_UTF8
        ? ::WideCharToMultiByte(
            codePage,
            0,
            keyword.data(),
            static_cast<int>(keyword.size()),
            reinterpret_cast<char*>(encoded.data()),
            requiredSize,
            nullptr,
            nullptr)
        : ::WideCharToMultiByte(
            codePage,
            0,
            keyword.data(),
            static_cast<int>(keyword.size()),
            reinterpret_cast<char*>(encoded.data()),
            requiredSize,
            "?",
            &usedDefaultChar);
    if (convertedSize != requiredSize || usedDefaultChar)
    {
        return {};
    }

    return encoded;
}

static void ResetHighlightOutputState(HIGHLIGHT_OUTPUT_STATE& state, const SERIAL_CONFIG& cfg)
{
    state.Rules.clear();
    state.Pending.clear();
    state.EncodingFormat = cfg.EncodingFormat;
    auto codePage = cfg.EncodingFormat == 0 ? CP_UTF8 : 936;
    for (const auto& rule : cfg.HighlightRules)
    {
        auto keyword = EncodeHighlightKeyword(rule.Keyword, codePage);
        if (keyword.empty() || rule.Color >= HIGHLIGHT_COLOR_COUNT)
        {
            continue;
        }

        state.Rules.push_back({std::move(keyword), rule.Color});
    }

    std::stable_sort(
        state.Rules.begin(),
        state.Rules.end(),
        [](const ENCODED_HIGHLIGHT_RULE& left, const ENCODED_HIGHLIGHT_RULE& right)
        {
            return left.Keyword.size() > right.Keyword.size();
        });
}

static void AppendAnsiSequence(std::vector<uint8_t>& output, const char* sequence)
{
    while (*sequence != '\0')
    {
        output.push_back(static_cast<uint8_t>(*sequence));
        ++sequence;
    }
}

static std::vector<uint8_t> BuildHighlightedOutput(
    HIGHLIGHT_OUTPUT_STATE& state,
    const uint8_t* data,
    size_t size,
    bool flush)
{
    if (state.Rules.empty())
    {
        if (size == 0)
        {
            return {};
        }
        return std::vector<uint8_t>(data, data + size);
    }

    if (size > 0)
    {
        state.Pending.insert(state.Pending.end(), data, data + size);
    }
    std::vector<uint8_t> output;
    size_t position = 0;
    while (position < state.Pending.size())
    {
        auto remainingSize = state.Pending.size() - position;
        if (!flush)
        {
            auto isKeywordPrefix = std::any_of(
                state.Rules.begin(),
                state.Rules.end(),
                [&state, position, remainingSize](const ENCODED_HIGHLIGHT_RULE& rule)
                {
                    return rule.Keyword.size() > remainingSize &&
                        std::equal(state.Pending.begin() + position, state.Pending.end(), rule.Keyword.begin());
                });
            if (isKeywordPrefix)
            {
                break;
            }
        }

        const ENCODED_HIGHLIGHT_RULE* matchedRule = nullptr;
        for (const auto& rule : state.Rules)
        {
            if (position + rule.Keyword.size() <= state.Pending.size() &&
                std::equal(rule.Keyword.begin(), rule.Keyword.end(), state.Pending.begin() + position))
            {
                matchedRule = &rule;
                break;
            }
        }

        if (matchedRule == nullptr)
        {
            output.push_back(state.Pending[position]);
            ++position;
            continue;
        }

        AppendAnsiSequence(output, GetHighlightColorSequence(matchedRule->Color));
        output.insert(output.end(), matchedRule->Keyword.begin(), matchedRule->Keyword.end());
        AppendAnsiSequence(output, "\033[39m");
        position += matchedRule->Keyword.size();
    }

    state.Pending.erase(state.Pending.begin(), state.Pending.begin() + position);
    return output;
}

static bool DecodeUtf8Text(const std::vector<uint8_t>& bytes, std::wstring& text)
{
    text.clear();
    if (bytes.empty())
    {
        return true;
    }

    auto textLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (textLength <= 0)
    {
        return false;
    }

    text.resize(static_cast<size_t>(textLength));
    return MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &text[0],
        textLength) == textLength;
}

typedef struct
{
    std::shared_ptr<std::vector<uint8_t>> Data;
    std::function<bool()> Started;
    std::function<void(const boost::system::error_code&)> Completed;
}SERIAL_WRITE_ITEM;

template <class TStream>
class ASYNC_WRITE_QUEUE
{
public:
    explicit ASYNC_WRITE_QUEUE(TStream& stream)
        : Stream(stream)
    {
    }

    void Enqueue(
        std::vector<uint8_t> data,
        std::function<bool()> started = {},
        std::function<void(const boost::system::error_code&)> completed = {})
    {
        if (Stopped)
            return;
        auto wasIdle = Queue.empty();
        Queue.push_back({
            std::make_shared<std::vector<uint8_t>>(std::move(data)),
            std::move(started),
            std::move(completed)
        });
        if (wasIdle)
        {
            StartNext();
        }
    }

    void Stop()
    {
        Stopped = true;
    }

private:
    void StartNext()
    {
        if (Stopped)
        {
            Queue.clear();
            return;
        }
        if (Queue.empty())
        {
            return;
        }

        auto item = Queue.front();
        if (item.Started && !item.Started())
        {
            Queue.pop_front();
            if (item.Completed)
            {
                item.Completed(boost::asio::error::operation_aborted);
            }
            StartNext();
            return;
        }

        if (item.Data->empty())
        {
            Queue.pop_front();
            if (item.Completed)
            {
                item.Completed({});
            }
            StartNext();
            return;
        }

        boost::asio::async_write(
            Stream,
            boost::asio::buffer(*item.Data),
            [this, item](const boost::system::error_code& ec, std::size_t)
            {
                Queue.pop_front();
                if (!Stopped && item.Completed)
                {
                    item.Completed(ec);
                }
                StartNext();
            });
    }

    TStream& Stream;
    std::deque<SERIAL_WRITE_ITEM> Queue;
    bool Stopped = false;
};

using SERIAL_WRITE_QUEUE = ASYNC_WRITE_QUEUE<boost::asio::serial_port>;
using CONSOLE_WRITE_QUEUE = ASYNC_WRITE_QUEUE<boost::asio::windows::stream_handle>;

class TERMINAL_DISPLAY
{
  public:
    TERMINAL_DISPLAY(boost::asio::io_service &ioctx, CONSOLE_WRITE_QUEUE &queue, SERIAL_CONFIG &config,
                     LOCAL_LINE_EDITOR &editor)
        : IoContext(ioctx), Queue(queue), Config(config), Editor(editor), ResizeTimer(ioctx), EchoTimer(ioctx)
    {
        ResetHighlightOutputState(Highlight, config);
    }

    UINT CodePage() const
    {
        return Config.EncodingFormat == 0 ? CP_UTF8 : 936;
    }

    void Start()
    {
        ApplyMode();
        WatchSize();
    }

    void ApplyMode()
    {
        Configure(Config);
    }

    void Configure(SERIAL_CONFIG next)
    {
        FlushEcho();
        auto pending = Formatter.Flush();
        auto output = BuildHighlightedOutput(Highlight, pending.data(), pending.size(), true);
        Write(std::move(output));
        if (Local)
            QueueText(EndFooter());
        Config = std::move(next);
        auto codePage = CodePage();
        Queue.Enqueue({}, [codePage]() {
            SetConsoleOutputCP(codePage);
            SetConsoleCP(codePage);
            return true;
        });
        Local = Config.EchoMode == 1;
        ResetHighlightOutputState(Highlight, Config);
        Editor.ResetDecoder(codePage);
        if (Local)
            BeginFooter();
    }

    void Receive(const uint8_t *data, size_t size, const std::string &time)
    {
        auto firstTime = EchoTime.empty() ? time : EchoTime;
        auto pending = Echo.Feed(data, size);
        ReceiveBytes(pending.data(), pending.size(), firstTime);
        ReceiveBytes(data, size, time);
        if (Echo.Waiting())
            EchoTime = firstTime;
        else
        {
            EchoTime.clear();
            boost::system::error_code ignored;
            EchoTimer.cancel(ignored);
        }
    }

    void Message(const std::string &text)
    {
        FlushEcho();
        Write(BuildHighlightedOutput(Highlight, nullptr, 0, true));
        Formatter.BeginLine();
        std::string output = "\r\n";
        if (Config.Timestamps)
            output += LocalTimestamp();
        output += text + "\r\n";
        Write(std::vector<uint8_t>(output.begin(), output.end()));
    }

    void Sent(const std::vector<uint8_t> &command, bool external)
    {
        std::string text = external ? "\033[96m[TX External] > " : "\033[96m> ";
        // 发送记录只显示文本，避免外部控制命令中的控制字符改变输入区
        for (auto byte : command)
            text += byte < 32 || byte == 127 ? ' ' : static_cast<char>(byte);
        Message(text + "\033[39m");
        if (Config.EchoMode == 1 && !external && !command.empty())
        {
            Echo.Expect(command);
            // 接收内容没有回显标记；只在发送后短时间内尝试完整匹配
            EchoTimer.expires_after(std::chrono::seconds(1));
            EchoTimer.async_wait([this](const boost::system::error_code &ec) {
                if (!ec)
                    FlushEcho();
            });
        }
    }

    void Redraw()
    {
        if (Local)
            QueueText("\033[?25l" + Editor.Render(Rows, Columns, CodePage()) + "\033[?25h");
    }

    bool IsStopped() const
    {
        return Stopped;
    }

    boost::system::error_code ExitError() const
    {
        return Failure;
    }

    void Cancel()
    {
        Stopped = true;
        boost::system::error_code ignored;
        ResizeTimer.cancel(ignored);
        EchoTimer.cancel(ignored);
    }

    void Stop(const boost::system::error_code& error = {})
    {
        if (Stopped)
            return;
        Failure = error;
        Cancel();
        FlushEcho();
        auto pending = Formatter.Flush();
        Write(BuildHighlightedOutput(Highlight, pending.data(), pending.size(), true));
        std::string end = Local ? EndFooter() : "";
        Local = false;
        Queue.Enqueue(std::vector<uint8_t>(end.begin(), end.end()), {},
                      [this](const boost::system::error_code &) { IoContext.stop(); });
    }

  private:
    void ReceiveBytes(const uint8_t *data, size_t size, const std::string &time)
    {
        if (!size)
            return;
        std::vector<uint8_t> bytes;
        if (Config.Timestamps || Local)
            bytes = Formatter.Feed(data, size, time, Config.Timestamps != 0, Local, CodePage());
        else
            bytes.assign(data, data + size);
        Write(BuildHighlightedOutput(Highlight, bytes.data(), bytes.size(), false));
    }

    void FlushEcho()
    {
        boost::system::error_code ignored;
        EchoTimer.cancel(ignored);
        auto pending = Echo.Flush();
        ReceiveBytes(pending.data(), pending.size(), EchoTime);
        EchoTime.clear();
    }

    void QueueText(const std::string &text)
    {
        Queue.Enqueue(std::vector<uint8_t>(text.begin(), text.end()));
    }

    void Write(std::vector<uint8_t> output)
    {
        if (output.empty())
            return;
        if (Local)
        {
            std::vector<uint8_t> wrapped;
            AppendAnsiSequence(wrapped, "\033[?25l\x1b\x38");
            wrapped.insert(wrapped.end(), output.begin(), output.end());
            AppendAnsiSequence(wrapped, "\x1b\x37");
            auto input = Editor.Render(Rows, Columns, CodePage());
            wrapped.insert(wrapped.end(), input.begin(), input.end());
            AppendAnsiSequence(wrapped, "\033[?25h");
            output = std::move(wrapped);
        }
        Queue.Enqueue(std::move(output));
    }

    std::string EndFooter() const
    {
        return "\033[" + std::to_string(Rows) + ";1H\033[2K\033[r\x1b\x38\033[0m\033[?2004l\033[?25h\r\n";
    }

    void BeginFooter()
    {
        CONSOLE_SCREEN_BUFFER_INFO info = {};
        if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        {
            Message("[Error] Cannot read console dimensions for local input");
            Stop();
            return;
        }
        Rows = (std::max)(3, static_cast<int>(info.srWindow.Bottom - info.srWindow.Top + 1));
        Columns = (std::max)(8, static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1));
        int logRow = (std::min)(Rows - 1, static_cast<int>(info.dwCursorPosition.Y - info.srWindow.Top + 2));
        QueueText("\033[?25l\r\n\033[1;" + std::to_string(Rows - 1) + "r\033[" + std::to_string(logRow) +
                  ";1H\x1b\x37\033[?2004h" + Editor.Render(Rows, Columns, CodePage()) + "\033[?25h");
    }

    void WatchSize()
    {
        if (Stopped)
            return;
        ResizeTimer.expires_after(std::chrono::milliseconds(200));
        ResizeTimer.async_wait([this](const boost::system::error_code &ec) {
            if (ec || Stopped)
                return;
            CONSOLE_SCREEN_BUFFER_INFO info = {};
            if (Local && GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) &&
                (Columns != info.srWindow.Right - info.srWindow.Left + 1 ||
                 Rows != info.srWindow.Bottom - info.srWindow.Top + 1))
            {
                QueueText(EndFooter());
                BeginFooter();
            }
            WatchSize();
        });
    }

    boost::asio::io_service &IoContext;
    CONSOLE_WRITE_QUEUE &Queue;
    SERIAL_CONFIG &Config;
    LOCAL_LINE_EDITOR &Editor;
    boost::asio::steady_timer ResizeTimer;
    boost::asio::steady_timer EchoTimer;
    HIGHLIGHT_OUTPUT_STATE Highlight;
    RECEIVE_FORMATTER Formatter;
    COMMAND_ECHO_FILTER Echo;
    std::string EchoTime;
    bool Local = false;
    bool Stopped = false;
    boost::system::error_code Failure;
    int Rows = 24;
    int Columns = 80;
};

typedef struct
{
    DWORD Status;
    DWORD CompletionReason;
    DWORD EncodingFormat;
    std::vector<uint8_t> Data;
}AUTOMATION_RESULT;

class AUTOMATION_SERVER
{
public:
    AUTOMATION_SERVER(
        boost::asio::io_service& ioctx,
        SERIAL_CONFIG& cfg,
        SERIAL_WRITE_QUEUE& writeQueue,
        TERMINAL_DISPLAY& display)
        : IoContext(ioctx),
          Config(cfg),
          WriteQueue(writeQueue),
          Display(display),
          Stopping(false),
          CommandPending(false),
          ServerInitialized(false),
          ServerAvailable(false),
          NextRequestId(0),
          ActiveRequestId(0),
          RequestStarted(false),
          RequestFinished(false),
          ReceivedData(false),
          Status(serial_automation::RESPONSE_STATUS_SUCCESS),
          CompletionReason(serial_automation::COMPLETION_REASON_NONE),
          EncodingFormat(0),
          IdleTimeoutMs(300)
    {
    }

    ~AUTOMATION_SERVER()
    {
        Stop();
    }

    bool Start()
    {
        if (Worker.joinable())
        {
            std::lock_guard<std::mutex> lock(ServerMutex);
            return ServerAvailable;
        }

        Stopping.store(false);
        {
            std::lock_guard<std::mutex> lock(ServerMutex);
            ServerInitialized = false;
            ServerAvailable = false;
        }
        Worker = std::thread([this]() { WorkerMain(); });
        std::unique_lock<std::mutex> lock(ServerMutex);
        ServerChanged.wait_for(
            lock,
            std::chrono::seconds(2),
            [this]() { return ServerInitialized; });
        return ServerInitialized && ServerAvailable;
    }

    void Stop()
    {
        if (!Worker.joinable())
        {
            return;
        }

        Stopping.store(true);
        ResponseChanged.notify_all();
        auto workerHandle = static_cast<HANDLE>(Worker.native_handle());
        // 取消可能早于工作线程进入同步 I/O，等待期间继续取消，避免 join 卡在管道读写
        do
        {
            CancelSynchronousIo(workerHandle);
        } while (WaitForSingleObject(workerHandle, 20) == WAIT_TIMEOUT);
        Worker.join();
    }

    bool IsCommandActive() const
    {
        return CommandPending.load();
    }

    void OnSerialData(const uint8_t* data, size_t size)
    {
        if (size == 0 || !CommandPending.load())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(ResponseMutex);
        if (!RequestStarted || RequestFinished)
        {
            return;
        }

        if (ResponseData.size() + size > serial_automation::kMaxResponseSize)
        {
            FinishRequestLocked(
                serial_automation::RESPONSE_STATUS_RESPONSE_TOO_LARGE,
                serial_automation::COMPLETION_REASON_ERROR);
            return;
        }

        ResponseData.insert(ResponseData.end(), data, data + size);
        ReceivedData = true;
        LastDataTime = std::chrono::steady_clock::now();
        if (!Prompt.empty() &&
            ResponseData.size() >= Prompt.size() &&
            std::equal(Prompt.rbegin(), Prompt.rend(), ResponseData.rbegin()))
        {
            FinishRequestLocked(
                serial_automation::RESPONSE_STATUS_SUCCESS,
                serial_automation::COMPLETION_REASON_PROMPT);
            return;
        }
        ResponseChanged.notify_all();
    }

private:
    static bool ReadExact(HANDLE pipe, void* data, DWORD size)
    {
        auto destination = static_cast<uint8_t*>(data);
        DWORD totalRead = 0;
        while (totalRead < size)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(pipe, destination + totalRead, size - totalRead, &bytesRead, nullptr) || bytesRead == 0)
            {
                return false;
            }
            totalRead += bytesRead;
        }
        return true;
    }

    static bool WriteExact(HANDLE pipe, const void* data, DWORD size)
    {
        auto source = static_cast<const uint8_t*>(data);
        DWORD totalWritten = 0;
        while (totalWritten < size)
        {
            DWORD bytesWritten = 0;
            if (!WriteFile(pipe, source + totalWritten, size - totalWritten, &bytesWritten, nullptr) || bytesWritten == 0)
            {
                return false;
            }
            totalWritten += bytesWritten;
        }
        return true;
    }

    static bool IsValidRequest(const serial_automation::REQUEST_HEADER& header)
    {
        return header.Magic == serial_automation::kProtocolMagic &&
            header.Version == serial_automation::kProtocolVersion &&
            header.CommandSize > 0 &&
            header.CommandSize <= serial_automation::kMaxTextSize &&
            header.PromptSize <= serial_automation::kMaxTextSize &&
            header.IdleTimeoutMs >= 10 &&
            header.IdleTimeoutMs <= 60000 &&
            header.TotalTimeoutMs >= header.IdleTimeoutMs &&
            header.TotalTimeoutMs <= 600000 &&
            header.LineEnding <= serial_automation::LINE_ENDING_CRLF;
    }

    void WorkerMain()
    {
        bool startupReported = false;
        while (!Stopping.load())
        {
            auto pipe = CreateNamedPipeW(
                serial_automation::kPipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                4096,
                4096,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                if (!startupReported)
                {
                    ReportStartup(false);
                }
                std::cerr << "\033[31mAutomation pipe error: " << GetLastError() << "\033[39m" << std::endl;
                return;
            }

            if (!startupReported)
            {
                ReportStartup(true);
                startupReported = true;
            }
            auto connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            if (connected && !Stopping.load())
            {
                HandleClient(pipe);
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    void ReportStartup(bool available)
    {
        std::lock_guard<std::mutex> lock(ServerMutex);
        ServerAvailable = available;
        ServerInitialized = true;
        ServerChanged.notify_all();
    }

    void HandleClient(HANDLE pipe)
    {
        serial_automation::REQUEST_HEADER requestHeader = {};
        if (!ReadExact(pipe, &requestHeader, sizeof(requestHeader)))
        {
            return;
        }

        AUTOMATION_RESULT result = {};
        if (!IsValidRequest(requestHeader))
        {
            result.Status = serial_automation::RESPONSE_STATUS_INVALID_REQUEST;
            result.CompletionReason = serial_automation::COMPLETION_REASON_ERROR;
        }
        else
        {
            std::vector<uint8_t> command(requestHeader.CommandSize);
            std::vector<uint8_t> prompt(requestHeader.PromptSize);
            if (!ReadExact(pipe, command.data(), requestHeader.CommandSize) ||
                (requestHeader.PromptSize > 0 && !ReadExact(pipe, prompt.data(), requestHeader.PromptSize)))
            {
                return;
            }
            result = ExecuteRequest(requestHeader, std::move(command), std::move(prompt));
        }

        serial_automation::RESPONSE_HEADER responseHeader = {};
        responseHeader.Magic = serial_automation::kProtocolMagic;
        responseHeader.Version = serial_automation::kProtocolVersion;
        responseHeader.Status = result.Status;
        responseHeader.CompletionReason = result.CompletionReason;
        responseHeader.EncodingFormat = result.EncodingFormat;
        responseHeader.DataSize = static_cast<DWORD>(result.Data.size());
        if (!WriteExact(pipe, &responseHeader, sizeof(responseHeader)))
        {
            return;
        }
        if (!result.Data.empty())
        {
            WriteExact(pipe, result.Data.data(), responseHeader.DataSize);
        }
        FlushFileBuffers(pipe);
    }

    AUTOMATION_RESULT ExecuteRequest(
        const serial_automation::REQUEST_HEADER& header,
        std::vector<uint8_t> commandUtf8,
        std::vector<uint8_t> promptUtf8)
    {
        const auto requestId = ++NextRequestId;
        const auto totalDeadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(header.TotalTimeoutMs);
        {
            std::lock_guard<std::mutex> lock(ResponseMutex);
            ActiveRequestId = requestId;
            RequestStarted = false;
            RequestFinished = false;
            ReceivedData = false;
            Status = serial_automation::RESPONSE_STATUS_SUCCESS;
            CompletionReason = serial_automation::COMPLETION_REASON_NONE;
            EncodingFormat = 0;
            IdleTimeoutMs = header.IdleTimeoutMs;
            ResponseData.clear();
            Prompt.clear();
        }
        CommandPending.store(true);

        IoContext.post(
            [this, requestId, header, commandUtf8 = std::move(commandUtf8), promptUtf8 = std::move(promptUtf8)]() mutable
            {
                StartRequestOnIoThread(requestId, header, std::move(commandUtf8), std::move(promptUtf8));
            });

        AUTOMATION_RESULT result = {};
        std::unique_lock<std::mutex> lock(ResponseMutex);
        while (!RequestFinished)
        {
            if (Stopping.load())
            {
                FinishRequestLocked(
                    serial_automation::RESPONSE_STATUS_SERVER_STOPPED,
                    serial_automation::COMPLETION_REASON_ERROR);
                break;
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= totalDeadline)
            {
                FinishRequestLocked(
                    serial_automation::RESPONSE_STATUS_TIMEOUT,
                    serial_automation::COMPLETION_REASON_TIMEOUT);
                break;
            }

            auto wakeTime = totalDeadline;
            if (RequestStarted && ReceivedData)
            {
                auto idleDeadline = LastDataTime + std::chrono::milliseconds(IdleTimeoutMs);
                if (now >= idleDeadline)
                {
                    FinishRequestLocked(
                        serial_automation::RESPONSE_STATUS_SUCCESS,
                        serial_automation::COMPLETION_REASON_IDLE);
                    break;
                }
                wakeTime = (std::min)(wakeTime, idleDeadline);
            }
            ResponseChanged.wait_until(lock, wakeTime);
        }

        result.Status = Status;
        result.CompletionReason = CompletionReason;
        result.EncodingFormat = EncodingFormat;
        result.Data = ResponseData;
        RequestStarted = false;
        lock.unlock();
        CommandPending.store(false);
        return result;
    }

    void StartRequestOnIoThread(
        std::uint64_t requestId,
        const serial_automation::REQUEST_HEADER& header,
        std::vector<uint8_t> commandUtf8,
        std::vector<uint8_t> promptUtf8)
    {
        {
            std::lock_guard<std::mutex> lock(ResponseMutex);
            if (Stopping.load() || requestId != ActiveRequestId || RequestFinished)
            {
                return;
            }
        }

        std::wstring commandText;
        std::wstring promptText;
        if (!DecodeUtf8Text(commandUtf8, commandText) || !DecodeUtf8Text(promptUtf8, promptText))
        {
            std::lock_guard<std::mutex> lock(ResponseMutex);
            FinishRequestLocked(
                serial_automation::RESPONSE_STATUS_INVALID_REQUEST,
                serial_automation::COMPLETION_REASON_ERROR);
            return;
        }

        auto encodingFormat = Config.EncodingFormat;
        auto codePage = encodingFormat == 0 ? CP_UTF8 : 936;
        auto command = EncodeHighlightKeyword(commandText, codePage);
        auto prompt = EncodeHighlightKeyword(promptText, codePage);
        if ((!commandText.empty() && command.empty()) || (!promptText.empty() && prompt.empty()))
        {
            std::lock_guard<std::mutex> lock(ResponseMutex);
            FinishRequestLocked(
                serial_automation::RESPONSE_STATUS_INVALID_REQUEST,
                serial_automation::COMPLETION_REASON_ERROR);
            return;
        }
        auto displayCommand = command;
        switch (header.LineEnding)
        {
        case serial_automation::LINE_ENDING_CR:
            command.push_back('\r');
            break;
        case serial_automation::LINE_ENDING_LF:
            command.push_back('\n');
            break;
        case serial_automation::LINE_ENDING_CRLF:
            command.push_back('\r');
            command.push_back('\n');
            break;
        }

        g_isCtrlAPressed = false;

        WriteQueue.Enqueue(
            std::move(command),
            [this, requestId, encodingFormat, prompt = std::move(prompt), displayCommand = std::move(displayCommand)]() mutable
            {
                std::lock_guard<std::mutex> lock(ResponseMutex);
                if (Stopping.load() || requestId != ActiveRequestId || RequestFinished)
                {
                    return false;
                }
                EncodingFormat = encodingFormat;
                Prompt = std::move(prompt);
                RequestStarted = true;
                Display.Sent(displayCommand, true);
                LastDataTime = std::chrono::steady_clock::now();
                ResponseChanged.notify_all();
                return true;
            },
            [this, requestId](const boost::system::error_code& ec)
            {
                if (!ec || ec == boost::asio::error::operation_aborted)
                {
                    return;
                }
                std::lock_guard<std::mutex> lock(ResponseMutex);
                if (requestId == ActiveRequestId && !RequestFinished)
                {
                    FinishRequestLocked(
                        serial_automation::RESPONSE_STATUS_SERIAL_ERROR,
                        serial_automation::COMPLETION_REASON_ERROR);
                }
            });
    }

    void FinishRequestLocked(DWORD status, DWORD completionReason)
    {
        Status = status;
        CompletionReason = completionReason;
        RequestFinished = true;
        ResponseChanged.notify_all();
    }

    boost::asio::io_service& IoContext;
    SERIAL_CONFIG& Config;
    SERIAL_WRITE_QUEUE& WriteQueue;
    TERMINAL_DISPLAY& Display;
    std::atomic<bool> Stopping;
    std::atomic<bool> CommandPending;
    std::thread Worker;
    std::mutex ServerMutex;
    std::condition_variable ServerChanged;
    bool ServerInitialized;
    bool ServerAvailable;
    std::mutex ResponseMutex;
    std::condition_variable ResponseChanged;
    std::uint64_t NextRequestId;
    std::uint64_t ActiveRequestId;
    bool RequestStarted;
    bool RequestFinished;
    bool ReceivedData;
    DWORD Status;
    DWORD CompletionReason;
    DWORD EncodingFormat;
    DWORD IdleTimeoutMs;
    std::vector<uint8_t> ResponseData;
    std::vector<uint8_t> Prompt;
    std::chrono::steady_clock::time_point LastDataTime;
};

static void DoSerialToConsole(boost::asio::serial_port &serialPort, std::vector<uint8_t> &buffer,
                              TERMINAL_DISPLAY &display, AUTOMATION_SERVER &automationServer)
{
    serialPort.async_read_some(boost::asio::buffer(buffer), [&serialPort, &buffer, &display, &automationServer](
                                                                const boost::system::error_code &ec, size_t size) {
        if (display.IsStopped())
            return;
        auto receivedAt = LocalTimestamp();
        if (ec)
        {
            // USB 拔出也可能返回 operation_aborted；只有已进入退出流程时才忽略取消
            display.Message("[Error] Serial receive: " + ErrorMessage(ec, display.CodePage()));
            display.Message("Serial connection closed. Check the connection and reopen this terminal profile.");
            display.Stop(ec);
            return;
        }
        automationServer.OnSerialData(buffer.data(), size);
        display.Receive(buffer.data(), size, receivedAt);
        DoSerialToConsole(serialPort, buffer, display, automationServer);
    });
}

static void DoConsoleToSerial(boost::asio::windows::stream_handle &consoleInput, SERIAL_WRITE_QUEUE &writeQueue,
                              std::vector<uint8_t> &buffer, SERIAL_CONFIG &cfg, TERMINAL_DISPLAY &display,
                              LOCAL_LINE_EDITOR &editor, COMMAND_HISTORY &history, AUTOMATION_SERVER &automationServer)
{
    consoleInput.async_read_some(boost::asio::buffer(buffer), [&consoleInput, &writeQueue, &buffer, &cfg, &display,
                                                               &editor, &history, &automationServer](
                                                                  const boost::system::error_code &ec, size_t size) {
        if (display.IsStopped())
            return;
        if (ec)
        {
            display.Message("[Error] Console input: " + ErrorMessage(ec, display.CodePage()));
            display.Stop(ec);
            return;
        }
        bool ignored = false;
        std::vector<uint8_t> rawInput;
        auto flushRaw = [&]() {
            if (!rawInput.empty())
            {
                writeQueue.Enqueue(std::move(rawInput));
                rawInput.clear();
            }
        };
        for (size_t i = 0; i < size; ++i)
        {
            auto byte = buffer[i];
            if (g_isCtrlAPressed)
            {
                g_isCtrlAPressed = false;
                flushRaw();
                auto next = cfg;
                if (byte == 24)
                {
                    auto answer = MessageBoxW(nullptr, L"确定要退出程序吗？", L"退出确认", MB_YESNO | MB_ICONQUESTION);
                    if (!RegisterConsoleControlHandler())
                    {
                        boost::system::error_code error(GetLastError(), boost::system::system_category());
                        display.Message("[Error] Console close handler: " + ErrorMessage(error, display.CodePage()));
                        display.Stop(error);
                        return;
                    }
                    if (answer == IDYES)
                    {
                        g_shouldExit = true;
                        consoleInput.cancel();
                        display.Stop();
                        return;
                    }
                }
                else if (byte == 3)
                {
                    next.EncodingFormat = cfg.EncodingFormat == 0 ? 1 : 0;
                    auto codePage = next.EncodingFormat == 0 ? CP_UTF8 : 936;
                    if (!editor.Text().empty() && EncodeTerminalText(editor.Text(), codePage).empty())
                    {
                        display.Message("[Error] Input cannot be represented in the selected encoding; draft kept");
                        continue;
                    }
                    display.Configure(next);
                    WriteSerialConfig(cfg);
                    display.Message(std::string("Encoding: ") + (cfg.EncodingFormat == 0 ? "UTF-8" : "GBK"));
                }
                else if (byte == 5)
                {
                    next.EchoMode = cfg.EchoMode == 0 ? 1 : 0;
                    display.Configure(next);
                    WriteSerialConfig(cfg);
                    display.Message(cfg.EchoMode ? "Input: local editor (history enabled)" : "Input: raw passthrough");
                }
                else if (byte == 20)
                {
                    next.Timestamps = cfg.Timestamps ? 0 : 1;
                    display.Configure(next);
                    WriteSerialConfig(cfg);
                    display.Message(cfg.Timestamps ? "Timestamps: on" : "Timestamps: off");
                }
                continue;
            }
            if (byte == 1)
            {
                g_isCtrlAPressed = true;
                continue;
            }
            if (automationServer.IsCommandActive())
            {
                ignored = true;
                continue;
            }
            if (cfg.EchoMode == 0)
            {
                rawInput.push_back(byte);
                continue;
            }

            auto action = editor.Feed(byte, display.CodePage());
            if (action == INPUT_ACTION::Submit)
            {
                auto command = editor.Text();
                auto bytes = EncodeTerminalText(command, display.CodePage());
                if (!command.empty() && bytes.empty())
                {
                    display.Message("[Error] Cannot encode command; draft kept");
                    continue;
                }
                auto displayed = bytes;
                // 与透传模式的 Enter 一致；CRLF 会让 Linux shell 再提交一次空行
                bytes.push_back('\r');
                editor.Clear();
                writeQueue.Enqueue(
                    std::move(bytes),
                    [&display, displayed]() {
                        display.Sent(displayed, false);
                        return true;
                    },
                    [&history, &display, command](const boost::system::error_code &error) {
                        if (error)
                        {
                            display.Message("[Error] Serial send: " + ErrorMessage(error, display.CodePage()));
                            display.Stop(error);
                            return;
                        }
                        auto saved = history.Remember(command);
                        if (saved)
                            display.Message("[Error] History was not saved (Windows error " + std::to_string(saved) +
                                            ")");
                        display.Redraw();
                    });
            }
            else if (action == INPUT_ACTION::Interrupt || action == INPUT_ACTION::EndOfFile)
                writeQueue.Enqueue({static_cast<uint8_t>(action == INPUT_ACTION::Interrupt ? 3 : 4)});
            else if (action == INPUT_ACTION::Rejected)
                display.Message("[Error] Input rejected: invalid character or command longer than 4096 characters");
        }
        flushRaw();
        if (ignored)
            display.Message("[External] Command is running; keyboard input ignored");
        display.Redraw();
        DoConsoleToSerial(consoleInput, writeQueue, buffer, cfg, display, editor, history, automationServer);
    });
}

static boost::system::error_code DoWork(boost::asio::io_service& ioctx, boost::asio::serial_port& serialPort, SERIAL_CONFIG& cfg)
{
    boost::system::error_code ec;
    boost::asio::windows::stream_handle stdinput(ioctx);
    boost::asio::windows::stream_handle stdoutput(ioctx);
    const auto kBufferSize = 1024;
    std::vector<uint8_t> serialPortRecvBuffer;
    std::vector<uint8_t> serialPortSendBuffer;
    serialPortRecvBuffer.resize(kBufferSize);
    serialPortSendBuffer.resize(kBufferSize);

    auto conin = CreateFile(L"CONIN$", FILE_GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    auto conout = CreateFile(L"CONOUT$", FILE_GENERIC_WRITE, FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);

    if (stdinput.assign(conin, ec))
        return ec;

    if (stdoutput.assign(conout, ec))
        return ec;

    HANDLE closeEvent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), g_consoleCloseRequested, GetCurrentProcess(), &closeEvent,
                         SYNCHRONIZE, FALSE, 0))
        return boost::system::error_code(GetLastError(), boost::system::system_category());
    boost::asio::windows::object_handle closeNotification(ioctx, closeEvent);
    bool cleaningUp = false;
    closeNotification.async_wait([&](const boost::system::error_code& error) {
        if (!error && !cleaningUp)
            ioctx.stop();
    });

    SERIAL_WRITE_QUEUE writeQueue(serialPort);
    CONSOLE_WRITE_QUEUE consoleWriteQueue(stdoutput);
    COMMAND_HISTORY history;
    std::wstring historyPath;
    auto historyError = CommandHistoryPath(historyPath);
    if (!historyError) historyError = history.Load(historyPath);
    LOCAL_LINE_EDITOR editor(history);
    TERMINAL_DISPLAY display(ioctx, consoleWriteQueue, cfg, editor);
    AUTOMATION_SERVER automationServer(ioctx, cfg, writeQueue, display);
    if (automationServer.Start())
    {
        std::cout << "\033[32mExternal control: \\.\\pipe\\SerialForWindowsTerminal\033[39m" << std::endl;
        std::cout << "\033[33mUse .\\SerialTerminalCtl.exe exec --line-ending cr \"ls\"\033[39m" << std::endl;
    }
    else
    {
        std::cout << "\033[93mExternal control unavailable; another terminal may own the pipe\033[39m" << std::endl;
    }

    display.Start();
    if (historyError) display.Message("[Error] Cannot load history (Windows error " + std::to_string(historyError) + ")");
    DoSerialToConsole(serialPort, serialPortRecvBuffer, display, automationServer);
    DoConsoleToSerial(stdinput, writeQueue, serialPortSendBuffer, cfg, display, editor, history, automationServer);

    // 重置退出标志
    g_shouldExit = false;
    
    ioctx.run(ec);
    cleaningUp = true;
    display.Cancel();
    writeQueue.Stop();
    consoleWriteQueue.Stop();
    boost::system::error_code ignored;
    closeNotification.cancel(ignored);
    stdinput.cancel(ignored);
    stdoutput.cancel(ignored);
    serialPort.cancel(ignored);
    automationServer.Stop();

    // 等取消完成后再释放接收缓冲区，取消回调不能重新发起读写
    ioctx.restart();
    ioctx.run(ignored);
    if (WaitForSingleObject(g_consoleCloseRequested, 0) == WAIT_OBJECT_0)
        return {};
    return display.ExitError() ? display.ExitError() : ec;
}

int wmain(int argc, const WCHAR* args[])
{
    boost::system::error_code ec;
    boost::asio::io_service ioctx;
    boost::asio::serial_port serialPort(ioctx);
    hInstance = GetModuleHandle(nullptr);

    g_consoleCloseRequested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_consoleCloseRequested)
        return GetLastError();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_consoleMainThread,
                         SYNCHRONIZE, FALSE, 0))
        return GetLastError();
    if (!RegisterConsoleControlHandler())
        return GetLastError();

    DWORD consoleMode = 0;
    auto conin = GetStdHandle(STD_INPUT_HANDLE);
    auto conout = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(conin, &consoleMode);
    consoleMode |= ENABLE_MOUSE_INPUT;
    consoleMode &= ~ENABLE_ECHO_INPUT;
    consoleMode &= ~ENABLE_PROCESSED_INPUT;
    consoleMode &= ~ENABLE_LINE_INPUT;
    consoleMode |= ENABLE_QUICK_EDIT_MODE;
    consoleMode |= ENABLE_WINDOW_INPUT;
    consoleMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(conin, consoleMode);

    GetConsoleMode(conout, &consoleMode);
    SetConsoleMode(conout, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);

    while (true)
    {
        auto hWndParent = ::GetForegroundWindow();
        if (hWndParent == nullptr)
            hWndParent = GetConsoleWindow();
        auto selection = DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_SETTING_DIALOG), hWndParent, SettingFunc, 1);
        if (!RegisterConsoleControlHandler())
            return GetLastError();
        if (WaitForSingleObject(g_consoleCloseRequested, 0) == WAIT_OBJECT_0)
            return ERROR_SUCCESS;
        if (selection == IDOK)
        {
            ioctx.restart();
            auto cfg = ReadSerialConfig();
            auto portName = std::string("COM") + std::to_string(cfg.Serial);
            if (serialPort.open(portName, ec))
            {
                std::cerr << "\033[31m" << "can not open " << portName << "\033[0m" <<std::endl;
                std::cerr << "\033[31m" << "error : " << ErrorMessage(ec, GetConsoleOutputCP()) << "\033[0m" << std::endl;
                continue;
            }
            if (InitializeSerialPort(serialPort, cfg, ec))
            {
                std::cerr << "\033[31m" << "can not initialize " << portName << "\033[0m" << std::endl;
                std::cerr << "\033[31m" << "error : " << ErrorMessage(ec, GetConsoleOutputCP()) << "\033[0m" << std::endl;
                continue;
            }
            else
            {
                // 根据用户选择的编码格式设置控制台
                if (cfg.EncodingFormat == 0) // UTF-8
                {
                    SetConsoleOutputCP(CP_UTF8);
                    SetConsoleCP(CP_UTF8);
                }
                else // GBK
                {
                    SetConsoleOutputCP(936);  // 936是GBK的代码页
                    SetConsoleCP(936);
                }

                    // 显示当前编码格式
                    std::cout << "\033[36mVersion: v1.0.0\033[0m" << std::endl;
                    std::cout << "\033[32mConsole encoding: " << (cfg.EncodingFormat == 0 ? "UTF-8" : "GBK") << "\033[0m" << std::endl;
                    std::cout << "\033[32mInput: " << (cfg.EchoMode == 0 ? "raw passthrough" : "local editor (history enabled)") << "\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+C to toggle encoding format\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+E to toggle raw/local input\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+T to toggle timestamps\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+X to exit\033[0m" << std::endl;

                    // 运行工作循环，传递配置
                    ec = DoWork(ioctx, serialPort, cfg);
                    if (ec)
                        return ec.value();
                    
                    // 检查是否需要退出程序
                    if (g_shouldExit)
                    {
                        break;
                    }
            }
            break;
        }
        else
        {
            return ERROR_CANCELLED;
        }
    }
    return ERROR_SUCCESS;
}

// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        CenterParentWindow(hDlg);
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

typedef struct
{
    std::vector<HIGHLIGHT_RULE> HighlightRules;
}SETTING_DIALOG_STATE;

static SETTING_DIALOG_STATE* GetSettingDialogState(HWND hDlg)
{
    return reinterpret_cast<SETTING_DIALOG_STATE*>(::GetWindowLongPtr(hDlg, GWLP_USERDATA));
}

static void RefreshHighlightRuleList(HWND hDlg, const std::vector<HIGHLIGHT_RULE>& rules)
{
    auto hWndList = GetDlgItem(hDlg, IDC_LIST_HIGHLIGHT);
    ListBox_ResetContent(hWndList);
    for (const auto& rule : rules)
    {
        auto displayText = rule.Keyword + L"    [" + GetHighlightColorName(rule.Color) + L"]";
        ListBox_AddString(hWndList, displayText.c_str());
    }
}

static void ShowSelectedHighlightRule(HWND hDlg)
{
    auto state = GetSettingDialogState(hDlg);
    auto selectedIndex = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_LIST_HIGHLIGHT));
    if (state == nullptr || selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= state->HighlightRules.size())
    {
        return;
    }

    const auto& rule = state->HighlightRules[static_cast<size_t>(selectedIndex)];
    SetDlgItemText(hDlg, IDC_EDIT_HIGHLIGHT_KEYWORD, rule.Keyword.c_str());
    ComboBox_SetCurSel(GetDlgItem(hDlg, IDC_COMBO_HIGHLIGHT_COLOR), static_cast<int>(rule.Color));
}

INT_PTR CALLBACK SettingFunc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
    {
        CenterParentWindow(hDlg);
        auto cfg = ReadSerialConfig();
        auto state = new SETTING_DIALOG_STATE;
        state->HighlightRules = cfg.HighlightRules;
        ::SetWindowLongPtr(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        auto hWndPort = GetDlgItem(hDlg, IDC_COMBO_PORT);
        UpdatePortControl(hDlg);
        auto portCount = ComboBox_GetCount(hWndPort);
        if (cfg.Serial != 0)
        {
            for (int i = 0; i < portCount; ++i)
            {
                auto com = (int)ComboBox_GetItemData(hWndPort, i);
                if (com == cfg.Serial)
                {
                    ComboBox_SetCurSel(hWndPort, i);
                    break;
                }
            }
        }
        else
        {
            ComboBox_SetCurSel(hWndPort, 0);
        }

        auto hWndBaudRate = GetDlgItem(hDlg, IDC_COMBO_SPEED);
        ComboBox_AddString(hWndBaudRate, L"50"); 
        ComboBox_AddString(hWndBaudRate, L"75");
        ComboBox_AddString(hWndBaudRate, L"100");
        ComboBox_AddString(hWndBaudRate, L"105");
        ComboBox_AddString(hWndBaudRate, L"300");
        ComboBox_AddString(hWndBaudRate, L"600");
        ComboBox_AddString(hWndBaudRate, L"1200");
        ComboBox_AddString(hWndBaudRate, L"2400");
        ComboBox_AddString(hWndBaudRate, L"4800");
        ComboBox_AddString(hWndBaudRate, L"9600");
        ComboBox_AddString(hWndBaudRate, L"19200");
        ComboBox_AddString(hWndBaudRate, L"38400");
        ComboBox_AddString(hWndBaudRate, L"57600");
        ComboBox_AddString(hWndBaudRate, L"115200");
        ComboBox_AddString(hWndBaudRate, L"128000");
        ComboBox_AddString(hWndBaudRate, L"256000");
        ComboBox_SetText(hWndBaudRate, std::to_wstring(cfg.BaudRate).c_str());

        auto hWndWordLength = GetDlgItem(hDlg, IDC_COMBO_WORD);
        ComboBox_AddString(hWndWordLength, L"4");
        ComboBox_AddString(hWndWordLength, L"5");
        ComboBox_AddString(hWndWordLength, L"6");
        ComboBox_AddString(hWndWordLength, L"7");
        ComboBox_AddString(hWndWordLength, L"8");
        ComboBox_AddString(hWndWordLength, L"9");
        ComboBox_AddString(hWndWordLength, L"10");
        ComboBox_SetCurSel(hWndWordLength, (int)(cfg.WordLength - 4));

        auto hWndStopBit = GetDlgItem(hDlg, IDC_COMBO_STOP);
        ComboBox_AddString(hWndStopBit, L"1");
        ComboBox_AddString(hWndStopBit, L"1.5");
        ComboBox_AddString(hWndStopBit, L"2");
        ComboBox_SetCurSel(hWndStopBit, (int)(cfg.StopBit));

        auto hWndParity = GetDlgItem(hDlg, IDC_COMBO_PARITY);
        ComboBox_AddString(hWndParity, L"无");
        ComboBox_AddString(hWndParity, L"奇");
        ComboBox_AddString(hWndParity, L"偶");
        ComboBox_SetCurSel(hWndParity, (int)(cfg.Parity));

        auto hWndFlowControl = GetDlgItem(hDlg, IDC_COMBO_FLOW_CONTROL);
        ComboBox_AddString(hWndFlowControl, L"无");
        ComboBox_AddString(hWndFlowControl, L"软件(Xon/Xoff)");
        ComboBox_AddString(hWndFlowControl, L"硬件");
        ComboBox_SetCurSel(hWndFlowControl, (int)(cfg.FlowControl));

        // 在WM_INITDIALOG处理部分，在FlowControl下拉框之后添加编码格式下拉框
        auto hWndEncoding = GetDlgItem(hDlg, IDC_COMBO_ENCODING);
        ComboBox_AddString(hWndEncoding, L"UTF-8");
        ComboBox_AddString(hWndEncoding, L"GBK");
        ComboBox_SetCurSel(hWndEncoding, (int)(cfg.EncodingFormat));

        // 添加回显模式复选框
        auto hWndEchoMode = GetDlgItem(hDlg, IDC_COMBO_ECHO);
        ComboBox_AddString(hWndEchoMode, L"字符透传");
        ComboBox_AddString(hWndEchoMode, L"本地编辑（历史建议）");
        ComboBox_SetCurSel(hWndEchoMode, (int)(cfg.EchoMode));
        CheckDlgButton(hDlg, IDC_CHECK_TIMESTAMPS, cfg.Timestamps ? BST_CHECKED : BST_UNCHECKED);

        auto hWndHighlightColor = GetDlgItem(hDlg, IDC_COMBO_HIGHLIGHT_COLOR);
        for (DWORD color = 0; color < HIGHLIGHT_COLOR_COUNT; ++color)
        {
            ComboBox_AddString(hWndHighlightColor, GetHighlightColorName(color));
        }
        ComboBox_SetCurSel(hWndHighlightColor, HIGHLIGHT_COLOR_RED);
        SendDlgItemMessage(hDlg, IDC_EDIT_HIGHLIGHT_KEYWORD, EM_SETLIMITTEXT, 64, 0);
        RefreshHighlightRuleList(hDlg, state->HighlightRules);

        return (INT_PTR)TRUE;
    }
    case WM_DEVICECHANGE:
        UpdatePortControl(hDlg);
        return (INT_PTR)TRUE;
    case WM_COMMAND:
    {
        auto command = LOWORD(wParam);
        if (command == IDC_LIST_HIGHLIGHT && HIWORD(wParam) == LBN_SELCHANGE)
        {
            ShowSelectedHighlightRule(hDlg);
            return (INT_PTR)TRUE;
        }
        if (command == IDC_BUTTON_HIGHLIGHT_ADD)
        {
            auto state = GetSettingDialogState(hDlg);
            if (state == nullptr)
            {
                return (INT_PTR)TRUE;
            }

            WCHAR keyword[65] = {0};
            GetDlgItemText(hDlg, IDC_EDIT_HIGHLIGHT_KEYWORD, keyword, _countof(keyword));
            if (keyword[0] == L'\0')
            {
                MessageBox(hDlg, L"请输入需要高亮的关键词。", L"关键词高亮", MB_OK | MB_ICONINFORMATION);
                return (INT_PTR)TRUE;
            }
            std::wstring keywordText(keyword);

            auto color = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_COMBO_HIGHLIGHT_COLOR));
            if (color < 0 || color >= HIGHLIGHT_COLOR_COUNT)
            {
                return (INT_PTR)TRUE;
            }

            auto duplicate = std::find_if(
                state->HighlightRules.begin(),
                state->HighlightRules.end(),
                [keywordText](const HIGHLIGHT_RULE& rule)
                {
                    return rule.Keyword == keywordText;
                });
            auto duplicateIndex = duplicate == state->HighlightRules.end()
                ? -1
                : static_cast<int>(duplicate - state->HighlightRules.begin());
            auto selectedIndex = duplicateIndex;
            if (duplicateIndex >= 0)
            {
                state->HighlightRules[static_cast<size_t>(selectedIndex)].Color = static_cast<DWORD>(color);
            }
            else
            {
                state->HighlightRules.push_back({keywordText, static_cast<DWORD>(color)});
                selectedIndex = static_cast<int>(state->HighlightRules.size() - 1);
            }

            RefreshHighlightRuleList(hDlg, state->HighlightRules);
            ListBox_SetCurSel(GetDlgItem(hDlg, IDC_LIST_HIGHLIGHT), selectedIndex);
            ShowSelectedHighlightRule(hDlg);
            return (INT_PTR)TRUE;
        }
        if (command == IDC_BUTTON_HIGHLIGHT_DELETE)
        {
            auto state = GetSettingDialogState(hDlg);
            auto selectedIndex = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_LIST_HIGHLIGHT));
            if (state == nullptr || selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= state->HighlightRules.size())
            {
                return (INT_PTR)TRUE;
            }

            state->HighlightRules.erase(state->HighlightRules.begin() + selectedIndex);
            RefreshHighlightRuleList(hDlg, state->HighlightRules);
            SetDlgItemText(hDlg, IDC_EDIT_HIGHLIGHT_KEYWORD, L"");
            ComboBox_SetCurSel(GetDlgItem(hDlg, IDC_COMBO_HIGHLIGHT_COLOR), HIGHLIGHT_COLOR_RED);
            return (INT_PTR)TRUE;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK)
            {
                SERIAL_CONFIG cfg = {};
                auto hWndPort = GetDlgItem(hDlg, IDC_COMBO_PORT);
                auto hWndBaudRate = GetDlgItem(hDlg, IDC_COMBO_SPEED);
                auto hWndWordLength = GetDlgItem(hDlg, IDC_COMBO_WORD);
                auto hWndStopBit = GetDlgItem(hDlg, IDC_COMBO_STOP);
                auto hWndParity = GetDlgItem(hDlg, IDC_COMBO_PARITY);
                auto hWndFlowControl = GetDlgItem(hDlg, IDC_COMBO_FLOW_CONTROL);

                WCHAR txtBuffer[32] = {0};
                auto curSel = ComboBox_GetCurSel(hWndPort);
                if (curSel >= 0)
                {
                    cfg.Serial = (DWORD)ComboBox_GetItemData(hWndPort, curSel);
                }
                else
                {
                    ComboBox_GetText(hWndPort, txtBuffer, 32);
                    cfg.Serial = std::wcstoul(txtBuffer + 3, nullptr, 10);
                }
                ComboBox_GetText(hWndBaudRate, txtBuffer, 32);
                cfg.BaudRate = std::wcstoul(txtBuffer, nullptr, 10);
                cfg.WordLength = ComboBox_GetCurSel(hWndWordLength) + 4;
                cfg.StopBit = ComboBox_GetCurSel(hWndStopBit);
                cfg.Parity = ComboBox_GetCurSel(hWndParity);
                cfg.FlowControl = ComboBox_GetCurSel(hWndFlowControl);
                // 获取编码格式下拉框的当前选择
                auto hWndEncoding = GetDlgItem(hDlg, IDC_COMBO_ENCODING);
                cfg.EncodingFormat = ComboBox_GetCurSel(hWndEncoding);

                // 获取回显模式复选框状态
                auto hWndEchoMode = GetDlgItem(hDlg, IDC_COMBO_ECHO);
                cfg.EchoMode = ComboBox_GetCurSel(hWndEchoMode);
                cfg.Timestamps = IsDlgButtonChecked(hDlg, IDC_CHECK_TIMESTAMPS) == BST_CHECKED ? 1 : 0;

                auto state = GetSettingDialogState(hDlg);
                if (state != nullptr)
                {
                    cfg.HighlightRules = state->HighlightRules;
                }

                WriteSerialConfig(cfg);
            }
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    case WM_DESTROY:
        delete GetSettingDialogState(hDlg);
        ::SetWindowLongPtr(hDlg, GWLP_USERDATA, 0);
        return (INT_PTR)TRUE;
    }
    return (INT_PTR)FALSE;
}
