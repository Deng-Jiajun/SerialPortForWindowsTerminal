// SerialForWindowsTerminal.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "SerialForWindowsTerminal.h"
#include "SerialAutomationProtocol.h"
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
std::vector<uint8_t> g_lineBuffer;

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
        WriteHighlightRules(hKey, cfg.HighlightRules);
        ::RegCloseKey(hKey);
    }
}

static void ToggleEncodingFormat(SERIAL_CONFIG& cfg)
{
    // 切换编码格式
    cfg.EncodingFormat = (cfg.EncodingFormat == 0) ? 1 : 0;
    
    // 应用新的编码格式
    if (cfg.EncodingFormat == 0) // UTF-8
    {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::cout << "\033[32mConsole encoding: UTF-8\033[0m" << std::endl;
    }
    else // GBK
    {
        SetConsoleOutputCP(936);
        SetConsoleCP(936);
        std::cout << "\033[32mConsole encoding: GBK\033[0m" << std::endl;
    }
    
    // 保存配置到注册表
    WriteSerialConfig(cfg);
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
        ReadHighlightRules(hKey, cfg.HighlightRules);
        ::RegCloseKey(hKey);
    }
    return cfg;
}

static void ToggleEchoMode(SERIAL_CONFIG& cfg)
{
    // 切换回显模式
    cfg.EchoMode = (cfg.EchoMode == 0) ? 1 : 0;
    
    // 清空行缓冲区
    g_lineBuffer.clear();
    
    // 显示当前回显模式
    std::cout << "\033[32mEcho mode: " << (cfg.EchoMode == 0 ? "Off" : "On") << "\033[0m" << std::endl;
    
    // 保存配置到注册表
    WriteSerialConfig(cfg);
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

static std::shared_ptr<HIGHLIGHT_OUTPUT_STATE> CreateHighlightOutputState(const SERIAL_CONFIG& cfg)
{
    auto state = std::make_shared<HIGHLIGHT_OUTPUT_STATE>();
    ResetHighlightOutputState(*state, cfg);
    return state;
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

private:
    void StartNext()
    {
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
                if (item.Completed)
                {
                    item.Completed(ec);
                }
                StartNext();
            });
    }

    TStream& Stream;
    std::deque<SERIAL_WRITE_ITEM> Queue;
};

using SERIAL_WRITE_QUEUE = ASYNC_WRITE_QUEUE<boost::asio::serial_port>;
using CONSOLE_WRITE_QUEUE = ASYNC_WRITE_QUEUE<boost::asio::windows::stream_handle>;

static void ShowKeyboardInputIgnored(CONSOLE_WRITE_QUEUE& consoleWriteQueue)
{
    std::vector<uint8_t> output;
    AppendAnsiSequence(
        output,
        "\a\r\n\033[93m[External] command is running; keyboard input ignored\033[39m\r\n");
    consoleWriteQueue.Enqueue(std::move(output));
}

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
        CONSOLE_WRITE_QUEUE& consoleWriteQueue)
        : IoContext(ioctx),
          Config(cfg),
          WriteQueue(writeQueue),
          ConsoleWriteQueue(consoleWriteQueue),
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
        CancelSynchronousIo(static_cast<HANDLE>(Worker.native_handle()));
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
        g_lineBuffer.clear();
        std::vector<uint8_t> displayOutput;
        AppendAnsiSequence(displayOutput, "\r\n\033[96m[External] > \033[39m");
        displayOutput.insert(displayOutput.end(), displayCommand.begin(), displayCommand.end());
        AppendAnsiSequence(displayOutput, "\r\n");
        ConsoleWriteQueue.Enqueue(std::move(displayOutput));

        WriteQueue.Enqueue(
            std::move(command),
            [this, requestId, encodingFormat, prompt = std::move(prompt)]() mutable
            {
                std::lock_guard<std::mutex> lock(ResponseMutex);
                if (Stopping.load() || requestId != ActiveRequestId || RequestFinished)
                {
                    return false;
                }
                EncodingFormat = encodingFormat;
                Prompt = std::move(prompt);
                RequestStarted = true;
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
    CONSOLE_WRITE_QUEUE& ConsoleWriteQueue;
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

static void DoSerialToConsole(
    boost::asio::serial_port& serialPort,
    CONSOLE_WRITE_QUEUE& consoleWriteQueue,
    std::vector<uint8_t>& buffer,
    SERIAL_CONFIG& cfg,
    const std::shared_ptr<HIGHLIGHT_OUTPUT_STATE>& highlightState,
    AUTOMATION_SERVER& automationServer)
{
    serialPort.async_read_some(
        boost::asio::buffer(buffer.data(), buffer.size()),
        [&serialPort, &consoleWriteQueue, &buffer, &cfg, highlightState, &automationServer](const boost::system::error_code& ec, std::size_t bytesTransferred)
        {
            if (ec)
            {
                auto remainingOutput = BuildHighlightedOutput(*highlightState, nullptr, 0, true);
                if (!remainingOutput.empty())
                {
                    consoleWriteQueue.Enqueue(std::move(remainingOutput));
                }
                std::cerr << "\033[31m" << "error : " << ec.message() << "\033[0m" << std::endl;
                return;
            }

            automationServer.OnSerialData(buffer.data(), bytesTransferred);
            std::vector<uint8_t> output;
            if (highlightState->EncodingFormat != cfg.EncodingFormat)
            {
                output = BuildHighlightedOutput(*highlightState, nullptr, 0, true);
                ResetHighlightOutputState(*highlightState, cfg);
            }
            auto currentOutput = BuildHighlightedOutput(*highlightState, buffer.data(), bytesTransferred, false);
            output.insert(output.end(), currentOutput.begin(), currentOutput.end());
            if (output.empty())
            {
                DoSerialToConsole(serialPort, consoleWriteQueue, buffer, cfg, highlightState, automationServer);
                return;
            }

            consoleWriteQueue.Enqueue(
                std::move(output),
                {},
                [](const boost::system::error_code& writeError)
                {
                    if (writeError)
                    {
                        std::cerr << "\033[31m" << "error : " << writeError.message() << "\033[0m" << std::endl;
                    }
                });
            DoSerialToConsole(serialPort, consoleWriteQueue, buffer, cfg, highlightState, automationServer);
        });
}

static void DoConsoleToSerial(
    boost::asio::windows::stream_handle& consoleInput,
    SERIAL_WRITE_QUEUE& writeQueue,
    CONSOLE_WRITE_QUEUE& consoleWriteQueue,
    std::vector<uint8_t>& buffer,
    SERIAL_CONFIG& cfg,
    AUTOMATION_SERVER& automationServer)
{
    consoleInput.async_read_some(
        boost::asio::buffer(buffer.data(), buffer.size()),
        [&consoleInput, &writeQueue, &consoleWriteQueue, &buffer, &cfg, &automationServer](const boost::system::error_code& ec, std::size_t bytesTransferred)
        {
            if (ec)
            {
                if (ec != boost::asio::error::operation_aborted)
                {
                    std::cerr << "\033[31m" << "error : " << ec.message() << "\033[0m" << std::endl;
                }
                return;
            }

            if (automationServer.IsCommandActive())
            {
                ShowKeyboardInputIgnored(consoleWriteQueue);
                if (!g_shouldExit)
                {
                    DoConsoleToSerial(consoleInput, writeQueue, consoleWriteQueue, buffer, cfg, automationServer);
                }
                return;
            }

            bool skipWrite = false;
            for (size_t i = 0; i < bytesTransferred; i++)
            {
                uint8_t ch = buffer[i];

                if (ch == 1)
                {
                    g_isCtrlAPressed = true;
                    skipWrite = true;
                    break;
                }

                if (g_isCtrlAPressed)
                {
                    g_isCtrlAPressed = false;
                    skipWrite = true;

                    if (ch == 24)
                    {
                        if (MessageBoxW(NULL, L"确定要退出程序吗？", L"退出确认", MB_YESNO | MB_ICONQUESTION) == IDYES)
                        {
                            g_shouldExit = true;
                            consoleInput.cancel();
                            exit(0);
                        }
                    }
                    else if (ch == 3)
                    {
                        ToggleEncodingFormat(cfg);
                    }
                    else if (ch == 5)
                    {
                        ToggleEchoMode(cfg);
                    }
                    break;
                }
            }

            if (!skipWrite && automationServer.IsCommandActive())
            {
                skipWrite = true;
                ShowKeyboardInputIgnored(consoleWriteQueue);
            }

            if (!skipWrite && cfg.EchoMode == 1)
            {
                skipWrite = true;
                for (size_t i = 0; i < bytesTransferred; i++)
                {
                    uint8_t ch = buffer[i];
                    if (ch == 8 || ch == 127)
                    {
                        if (!g_lineBuffer.empty())
                        {
                            g_lineBuffer.pop_back();
                            std::cout << "\b \b";
                        }
                        continue;
                    }

                    if (ch == '\r' || ch == '\n')
                    {
                        g_lineBuffer.push_back('\r');
                        g_lineBuffer.push_back('\n');
                        writeQueue.Enqueue(g_lineBuffer);
                        g_lineBuffer.clear();
                        std::cout << "\n";
                        continue;
                    }

                    g_lineBuffer.push_back(ch);
                    std::cout << static_cast<char>(ch);
                    std::cout.flush();
                }
            }

            if (!skipWrite && cfg.EchoMode == 0)
            {
                writeQueue.Enqueue(std::vector<uint8_t>(buffer.begin(), buffer.begin() + bytesTransferred));
            }

            if (!g_shouldExit)
            {
                DoConsoleToSerial(consoleInput, writeQueue, consoleWriteQueue, buffer, cfg, automationServer);
            }
        }
    );
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

    SERIAL_WRITE_QUEUE writeQueue(serialPort);
    CONSOLE_WRITE_QUEUE consoleWriteQueue(stdoutput);
    AUTOMATION_SERVER automationServer(ioctx, cfg, writeQueue, consoleWriteQueue);
    if (automationServer.Start())
    {
        std::cout << "\033[32mExternal control: \\.\\pipe\\SerialForWindowsTerminal\033[39m" << std::endl;
        std::cout << "\033[33mUse .\\SerialTerminalCtl.exe exec \"ls\"\033[39m" << std::endl;
    }
    else
    {
        std::cout << "\033[93mExternal control unavailable; another terminal may own the pipe\033[39m" << std::endl;
    }

    // 串口接收内容在写入终端前应用关键词高亮规则
    DoSerialToConsole(
        serialPort,
        consoleWriteQueue,
        serialPortRecvBuffer,
        cfg,
        CreateHighlightOutputState(cfg),
        automationServer);
    // 对于输入到串口的流，传递配置指针以支持快捷键处理
    DoConsoleToSerial(stdinput, writeQueue, consoleWriteQueue, serialPortSendBuffer, cfg, automationServer);
    
    // 重置退出标志
    g_shouldExit = false;
    
    ioctx.run(ec);
    automationServer.Stop();
    return ec;
}

int wmain(int argc, const WCHAR* args[])
{
    boost::system::error_code ec;
    boost::asio::io_service ioctx;
    boost::asio::serial_port serialPort(ioctx);
    hInstance = GetModuleHandle(nullptr);

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
        if (DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_SETTING_DIALOG), hWndParent, SettingFunc, 1) == IDOK)
        {
            ioctx.restart();
            auto cfg = ReadSerialConfig();
            auto portName = std::string("COM") + std::to_string(cfg.Serial);
            if (serialPort.open(portName, ec))
            {
                std::cerr << "\033[31m" << "can not open " << portName << "\033[0m" <<std::endl;
                std::cerr << "\033[31m" << "error : " << ec.message() << "\033[0m" << std::endl;
                continue;
            }
            if (InitializeSerialPort(serialPort, cfg, ec))
            {
                std::cerr << "\033[31m" << "can not initialize " << portName << "\033[0m" << std::endl;
                std::cerr << "\033[31m" << "error : " << ec.message() << "\033[0m" << std::endl;
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
                    std::cout << "\033[32mEcho mode: " << (cfg.EchoMode == 0 ? "Off" : "On") << "\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+C to toggle encoding format\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+E to toggle echo mode\033[0m" << std::endl;
                    std::cout << "\033[33mPress Ctrl+A then Ctrl+X to exit\033[0m" << std::endl;

                    // 运行工作循环，传递配置
                    DoWork(ioctx, serialPort, cfg);
                    
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
        ComboBox_AddString(hWndEchoMode, L"关闭");
        ComboBox_AddString(hWndEchoMode, L"开启");
        ComboBox_SetCurSel(hWndEchoMode, (int)(cfg.EchoMode));

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
