#include <windows.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "..\SerialAutomationProtocol.h"

struct COMMAND_OPTIONS
{
    std::wstring Command;
    std::wstring Prompt;
    DWORD IdleTimeoutMs = 300;
    DWORD TotalTimeoutMs = 5000;
    DWORD LineEnding = serial_automation::LINE_ENDING_CRLF;
};

static void PrintUsage()
{
    std::wcerr << L"Usage:\n"
        L"  .\\SerialTerminalCtl.exe exec [options] <command>\n\n"
        L"Options:\n"
        L"  --prompt <text>       Complete when the response ends with this text.\n"
        L"  --idle <ms>           Complete after response idle time (default: 300).\n"
        L"  --timeout <ms>        Total timeout (default: 5000).\n"
        L"  --line-ending <type>  none, cr, lf, or crlf (default: crlf).\n"
        L"  --                    End options; the remaining text is the command.\n\n"
        L"Example:\n"
        L"  .\\SerialTerminalCtl.exe exec --prompt \"# \" \"ls -l\"\n";
}

static bool ParseDword(const wchar_t* value, DWORD& result)
{
    wchar_t* end = nullptr;
    auto parsed = wcstoul(value, &end, 10);
    if (value == end || *end != L'\0' || parsed > MAXDWORD)
    {
        return false;
    }
    result = static_cast<DWORD>(parsed);
    return true;
}

static bool ParseLineEnding(const std::wstring& value, DWORD& result)
{
    if (value == L"none")
    {
        result = serial_automation::LINE_ENDING_NONE;
    }
    else if (value == L"cr")
    {
        result = serial_automation::LINE_ENDING_CR;
    }
    else if (value == L"lf")
    {
        result = serial_automation::LINE_ENDING_LF;
    }
    else if (value == L"crlf")
    {
        result = serial_automation::LINE_ENDING_CRLF;
    }
    else
    {
        return false;
    }
    return true;
}

static bool ParseArguments(int argc, const wchar_t* argv[], COMMAND_OPTIONS& options)
{
    if (argc < 3 || std::wstring(argv[1]) != L"exec")
    {
        return false;
    }

    int index = 2;
    while (index < argc)
    {
        auto argument = std::wstring(argv[index]);
        if (argument == L"--")
        {
            ++index;
            break;
        }
        if (argument == L"--prompt" || argument == L"--idle" ||
            argument == L"--timeout" || argument == L"--line-ending")
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            auto value = std::wstring(argv[index + 1]);
            if (argument == L"--prompt")
            {
                options.Prompt = value;
            }
            else if (argument == L"--idle")
            {
                if (!ParseDword(value.c_str(), options.IdleTimeoutMs))
                {
                    return false;
                }
            }
            else if (argument == L"--timeout")
            {
                if (!ParseDword(value.c_str(), options.TotalTimeoutMs))
                {
                    return false;
                }
            }
            else if (!ParseLineEnding(value, options.LineEnding))
            {
                return false;
            }
            index += 2;
            continue;
        }
        if (argument.size() > 2 && argument[0] == L'-' && argument[1] == L'-')
        {
            return false;
        }
        break;
    }

    for (; index < argc; ++index)
    {
        if (!options.Command.empty())
        {
            options.Command.push_back(L' ');
        }
        options.Command += argv[index];
    }

    return !options.Command.empty() &&
        options.IdleTimeoutMs >= 10 &&
        options.IdleTimeoutMs <= 60000 &&
        options.TotalTimeoutMs >= options.IdleTimeoutMs &&
        options.TotalTimeoutMs <= 600000;
}

static std::vector<uint8_t> WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    auto size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::vector<uint8_t> result(static_cast<size_t>(size));
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        reinterpret_cast<char*>(result.data()),
        size,
        nullptr,
        nullptr);
    return result;
}

static std::vector<uint8_t> ConvertToUtf8(const std::vector<uint8_t>& data, DWORD encodingFormat)
{
    if (data.empty() || encodingFormat == 0)
    {
        return data;
    }

    auto wideSize = MultiByteToWideChar(
        936,
        0,
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),
        nullptr,
        0);
    if (wideSize <= 0)
    {
        return data;
    }
    std::wstring wideText(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(
        936,
        0,
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),
        &wideText[0],
        wideSize);
    return WideToUtf8(wideText);
}

static void WriteUtf8Output(const std::vector<uint8_t>& data)
{
    if (data.empty())
    {
        return;
    }

    auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    if (GetConsoleMode(output, &consoleMode))
    {
        auto wideSize = MultiByteToWideChar(
            CP_UTF8,
            0,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()),
            nullptr,
            0);
        if (wideSize > 0)
        {
            std::wstring wideText(static_cast<size_t>(wideSize), L'\0');
            MultiByteToWideChar(
                CP_UTF8,
                0,
                reinterpret_cast<const char*>(data.data()),
                static_cast<int>(data.size()),
                &wideText[0],
                wideSize);
            DWORD charactersWritten = 0;
            WriteConsoleW(output, wideText.data(), static_cast<DWORD>(wideText.size()), &charactersWritten, nullptr);
            return;
        }
    }

    DWORD bytesWritten = 0;
    WriteFile(output, data.data(), static_cast<DWORD>(data.size()), &bytesWritten, nullptr);
}

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

static HANDLE ConnectToTerminal()
{
    auto deadline = GetTickCount64() + 2000;
    while (true)
    {
        auto pipe = CreateFileW(
            serial_automation::kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            return pipe;
        }

        auto error = GetLastError();
        auto now = GetTickCount64();
        if (now >= deadline || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY))
        {
            SetLastError(error);
            return INVALID_HANDLE_VALUE;
        }
        if (error == ERROR_PIPE_BUSY)
        {
            WaitNamedPipeW(serial_automation::kPipeName, static_cast<DWORD>(deadline - now));
        }
        else
        {
            Sleep(50);
        }
    }
}

static int GetExitCode(DWORD status)
{
    if (status == serial_automation::RESPONSE_STATUS_SUCCESS)
    {
        return 0;
    }
    if (status == serial_automation::RESPONSE_STATUS_TIMEOUT)
    {
        return 2;
    }
    return 3;
}

int wmain(int argc, const wchar_t* argv[])
{
    COMMAND_OPTIONS options;
    if (!ParseArguments(argc, argv, options))
    {
        PrintUsage();
        return 64;
    }

    auto command = WideToUtf8(options.Command);
    auto prompt = WideToUtf8(options.Prompt);
    if (command.empty() || command.size() > serial_automation::kMaxTextSize ||
        prompt.size() > serial_automation::kMaxTextSize)
    {
        std::cerr << "Command or prompt is too long." << std::endl;
        return 64;
    }

    auto pipe = ConnectToTerminal();
    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Serial terminal is not running or the automation pipe is busy. Windows error: "
            << GetLastError() << std::endl;
        return 3;
    }

    serial_automation::REQUEST_HEADER request = {};
    request.Magic = serial_automation::kProtocolMagic;
    request.Version = serial_automation::kProtocolVersion;
    request.CommandSize = static_cast<DWORD>(command.size());
    request.PromptSize = static_cast<DWORD>(prompt.size());
    request.IdleTimeoutMs = options.IdleTimeoutMs;
    request.TotalTimeoutMs = options.TotalTimeoutMs;
    request.LineEnding = options.LineEnding;

    auto requestWritten = WriteExact(pipe, &request, sizeof(request)) &&
        WriteExact(pipe, command.data(), request.CommandSize) &&
        (prompt.empty() || WriteExact(pipe, prompt.data(), request.PromptSize));
    if (!requestWritten)
    {
        std::cerr << "Failed to send the request. Windows error: " << GetLastError() << std::endl;
        CloseHandle(pipe);
        return 3;
    }

    serial_automation::RESPONSE_HEADER response = {};
    if (!ReadExact(pipe, &response, sizeof(response)) ||
        response.Magic != serial_automation::kProtocolMagic ||
        response.Version != serial_automation::kProtocolVersion ||
        response.DataSize > serial_automation::kMaxResponseSize)
    {
        std::cerr << "Invalid or incomplete response from the serial terminal." << std::endl;
        CloseHandle(pipe);
        return 3;
    }

    std::vector<uint8_t> responseData(response.DataSize);
    if (response.DataSize > 0 && !ReadExact(pipe, responseData.data(), response.DataSize))
    {
        std::cerr << "Incomplete response data from the serial terminal." << std::endl;
        CloseHandle(pipe);
        return 3;
    }
    CloseHandle(pipe);

    auto utf8Output = ConvertToUtf8(responseData, response.EncodingFormat);
    WriteUtf8Output(utf8Output);

    if (response.Status != serial_automation::RESPONSE_STATUS_SUCCESS)
    {
        std::cerr << "Serial command failed. Status: " << response.Status
            << ", completion reason: " << response.CompletionReason << std::endl;
    }
    return GetExitCode(response.Status);
}
