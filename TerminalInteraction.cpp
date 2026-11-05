#include "framework.h"
#include "TerminalInteraction.h"
#include <shlobj.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

const size_t COMMAND_HISTORY::MaxEntries;
const size_t COMMAND_HISTORY::MaxCommandLength;

std::string LocalTimestamp()
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    char text[32];
    sprintf_s(text, "[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return text;
}

std::vector<uint8_t> EncodeTerminalText(const std::wstring &text, UINT codePage)
{
    if (text.empty())
        return {};
    BOOL usedDefault = FALSE;
    auto defaultFlag = codePage == CP_UTF8 ? nullptr : &usedDefault;
    int size =
        WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, defaultFlag);
    if (size <= 0 || usedDefault)
        return {};
    std::vector<uint8_t> bytes(size);
    int written = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()),
                                      reinterpret_cast<char *>(bytes.data()), size, nullptr, defaultFlag);
    if (written != size || usedDefault)
        return {};
    return bytes;
}

static bool DecodeText(const std::string &bytes, UINT codePage, std::wstring &text)
{
    if (bytes.empty())
    {
        text.clear();
        return true;
    }
    int size =
        MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0)
        return false;
    text.resize(size);
    return MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), &text[0],
                               size) == size;
}

DWORD CommandHistoryPath(std::wstring &path)
{
    PWSTR folder = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
    if (FAILED(result))
        return HRESULT_CODE(result);
    path = std::wstring(folder) + L"\\SerialForWindowsTerminal\\history.txt";
    CoTaskMemFree(folder);
    return ERROR_SUCCESS;
}

static void AddHistory(std::vector<std::wstring> &entries, const std::wstring &command)
{
    entries.erase(std::remove(entries.begin(), entries.end(), command), entries.end());
    entries.push_back(command);
    if (entries.size() > COMMAND_HISTORY::MaxEntries)
        entries.erase(entries.begin());
}

static DWORD ReadHistory(const std::wstring &path, std::vector<std::wstring> &entries)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    LARGE_INTEGER size;
    DWORD error = ERROR_SUCCESS;
    std::string contents;
    if (!GetFileSizeEx(file, &size))
        error = GetLastError();
    else if (size.QuadPart > 8 * 1024 * 1024)
        error = ERROR_FILE_TOO_LARGE;
    else
    {
        contents.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!contents.empty() && !ReadFile(file, &contents[0], static_cast<DWORD>(contents.size()), &read, nullptr))
            error = GetLastError();
        else if (read != contents.size())
            error = ERROR_HANDLE_EOF;
    }
    CloseHandle(file);
    if (error)
        return error;
    size_t start = contents.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    while (start < contents.size())
    {
        size_t end = contents.find('\n', start);
        if (end == std::string::npos)
            end = contents.size();
        auto bytes = contents.substr(start, end - start);
        if (!bytes.empty() && bytes.back() == '\r')
            bytes.pop_back();
        std::wstring command;
        if (!DecodeText(bytes, CP_UTF8, command))
            return ERROR_NO_UNICODE_TRANSLATION;
        if (command.size() > COMMAND_HISTORY::MaxCommandLength ||
            std::any_of(command.begin(), command.end(), [](wchar_t ch) { return ch < L' ' || ch == 127; }))
            return ERROR_INVALID_DATA;
        if (!command.empty())
            AddHistory(entries, command);
        start = end + 1;
    }
    return ERROR_SUCCESS;
}

DWORD COMMAND_HISTORY::Load(const std::wstring &path)
{
    Path = path;
    std::vector<std::wstring> loaded;
    DWORD error = ReadHistory(path, loaded);
    if (!error)
        Commands = std::move(loaded);
    return error;
}

DWORD COMMAND_HISTORY::Remember(const std::wstring &command)
{
    // 只保存本地命令；前导空格提供明确的不记录方式
    if (command.empty() || command.front() == L' ')
        return ERROR_SUCCESS;
    if (command.size() > MaxCommandLength ||
        std::any_of(command.begin(), command.end(), [](wchar_t ch) { return ch < L' ' || ch == 127; }))
        return ERROR_INVALID_DATA;
    AddHistory(Commands, command);
    AddHistory(Unsaved, command);
    if (Path.empty())
        return ERROR_PATH_NOT_FOUND;
    auto separator = Path.find_last_of(L"\\/");
    if (separator != std::wstring::npos && !CreateDirectoryW(Path.substr(0, separator).c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return GetLastError();

    // 多个串口窗口共享历史：锁定后重新读入，避免覆盖其他窗口刚保存的命令
    HANDLE lock = CreateFileW((Path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE)
        return GetLastError();
    OVERLAPPED position = {};
    if (!LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &position))
    {
        DWORD error = GetLastError();
        CloseHandle(lock);
        return error;
    }
    std::vector<std::wstring> merged;
    DWORD error = ReadHistory(Path, merged);
    if (!error)
    {
        for (const auto &entry : Unsaved)
            AddHistory(merged, entry);
        std::string contents;
        for (const auto &entry : merged)
        {
            auto bytes = EncodeTerminalText(entry, CP_UTF8);
            contents.append(bytes.begin(), bytes.end());
            contents += "\r\n";
        }
        auto temporary = Path + L".tmp";
        HANDLE file =
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            error = GetLastError();
        else
        {
            DWORD written = 0;
            if (!WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr))
                error = GetLastError();
            else if (written != contents.size())
                error = ERROR_WRITE_FAULT;
            if (!error && !FlushFileBuffers(file))
                error = GetLastError();
            CloseHandle(file);
            if (!error)
            {
                BOOL saved = GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES
                                 ? ReplaceFileW(Path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
                                 : MoveFileExW(temporary.c_str(), Path.c_str(), MOVEFILE_WRITE_THROUGH);
                if (!saved)
                    error = GetLastError();
            }
            if (error)
                DeleteFileW(temporary.c_str());
        }
        if (!error)
        {
            Commands = std::move(merged);
            Unsaved.clear();
        }
    }
    UnlockFileEx(lock, 0, 1, 0, &position);
    CloseHandle(lock);
    return error;
}

void COMMAND_ECHO_FILTER::Expect(const std::vector<uint8_t> &command)
{
    Expected = command;
    Matched = 0;
    SkipLF = false;
}

std::vector<uint8_t> COMMAND_ECHO_FILTER::Flush()
{
    std::vector<uint8_t> output(Expected.begin(), Expected.begin() + Matched);
    Expected.clear();
    Matched = 0;
    SkipLF = false;
    return output;
}

std::vector<uint8_t> COMMAND_ECHO_FILTER::Feed(const uint8_t *&data, size_t &size)
{
    while (size)
    {
        if (SkipLF)
        {
            SkipLF = false;
            if (*data == '\n')
            {
                ++data;
                --size;
            }
        }
        if (Expected.empty())
            break;
        if (Matched == Expected.size())
        {
            if (*data != '\r' && *data != '\n')
                return Flush();
            SkipLF = *data == '\r';
            Expected.clear();
            Matched = 0;
        }
        else
        {
            if (*data != Expected[Matched])
                return Flush();
            ++Matched;
        }
        ++data;
        --size;
    }
    return {};
}

static size_t EncodedCharacterSize(uint8_t byte, UINT codePage)
{
    if (codePage != CP_UTF8)
        return IsDBCSLeadByteEx(codePage, byte) ? 2 : 1;
    if (byte >= 0xc2 && byte <= 0xdf)
        return 2;
    if (byte >= 0xe0 && byte <= 0xef)
        return 3;
    if (byte >= 0xf0 && byte <= 0xf4)
        return 4;
    return 1;
}

static bool EscapeFinished(const std::string &escape)
{
    if (escape.size() < 2)
        return false;
    if (escape[1] == '[')
        return escape.size() >= 3 && escape.back() >= '@' && escape.back() <= '~';
    if (escape[1] == ']' || escape[1] == 'P' || escape[1] == '^' || escape[1] == '_')
        return (escape[1] == ']' && escape.back() == '\a') ||
               (escape.size() >= 3 && escape[escape.size() - 2] == '\033' && escape.back() == '\\');
    if (escape[1] == '(' || escape[1] == ')')
        return escape.size() >= 3;
    return true;
}

std::vector<uint8_t> RECEIVE_FORMATTER::Feed(const uint8_t *data, size_t size, const std::string &time, bool timestamps,
                                             bool logMode, UINT codePage)
{
    std::vector<uint8_t> output;
    for (size_t i = 0; i < size; ++i)
    {
        uint8_t byte = data[i];
        if (!Escape.empty())
        {
            Escape += static_cast<char>(byte);
            if (EscapeFinished(Escape) || Escape.size() >= 4096)
            {
                if (!logMode || (Escape.size() >= 3 && Escape[1] == '[' && Escape.back() == 'm'))
                    output.insert(output.end(), Escape.begin(), Escape.end());
                Escape.clear();
            }
            continue;
        }
        if (byte == '\r' || byte == '\n')
        {
            if (!Character.empty())
            {
                if (LineStart && timestamps)
                    output.insert(output.end(), LineTime.begin(), LineTime.end());
                output.push_back('?');
                Character.clear();
            }
            if (!(byte == '\n' && PreviousCR))
            {
                output.push_back('\r');
                output.push_back('\n');
            }
            PreviousCR = byte == '\r';
            LineStart = true;
            LineTime.clear();
            continue;
        }
        PreviousCR = false;
        if (LineStart && LineTime.empty())
            LineTime = time;
        if (Character.empty() && byte == '\033')
        {
            Escape = "\033";
            continue;
        }
        if (Character.empty() && byte < 32 && byte != '\t')
        {
            if (!logMode)
                output.push_back(byte);
            continue;
        }
        if (Character.empty())
            CharacterSize = EncodedCharacterSize(byte, codePage);
        Character += static_cast<char>(byte);
        if (Character.size() < CharacterSize)
            continue;
        if (LineStart && timestamps)
            output.insert(output.end(), LineTime.begin(), LineTime.end());
        LineStart = false;
        output.insert(output.end(), Character.begin(), Character.end());
        Character.clear();
    }
    return output;
}

std::vector<uint8_t> RECEIVE_FORMATTER::Flush()
{
    // 不把未完成的控制序列写给终端，否则随后重绘输入行会成为该序列的一部分
    std::vector<uint8_t> output;
    if (!Character.empty())
        output.push_back('?');
    Reset();
    return output;
}

void RECEIVE_FORMATTER::Reset()
{
    *this = RECEIVE_FORMATTER();
}

void RECEIVE_FORMATTER::BeginLine()
{
    LineStart = true;
    PreviousCR = false;
    if (Character.empty() && Escape.empty())
        LineTime.clear();
}

static size_t NextCharacter(const std::wstring &text, size_t position)
{
    if (position >= text.size())
        return text.size();
    if (text[position] >= 0xd800 && text[position] <= 0xdbff && position + 1 < text.size() &&
        text[position + 1] >= 0xdc00 && text[position + 1] <= 0xdfff)
        return position + 2;
    return position + 1;
}

static size_t PreviousCharacter(const std::wstring &text, size_t position)
{
    if (position == 0)
        return 0;
    --position;
    if (position > 0 && text[position] >= 0xdc00 && text[position] <= 0xdfff && text[position - 1] >= 0xd800 &&
        text[position - 1] <= 0xdbff)
        --position;
    return position;
}

void LOCAL_LINE_EDITOR::Edit()
{
    Browsing = false;
}

INPUT_ACTION LOCAL_LINE_EDITOR::Insert(const std::wstring &text)
{
    if (Line.size() + text.size() > COMMAND_HISTORY::MaxCommandLength)
        return INPUT_ACTION::Rejected;
    Edit();
    Line.insert(Cursor, text);
    Cursor += text.size();
    return INPUT_ACTION::None;
}

void LOCAL_LINE_EDITOR::ResetDecoder(UINT codePage)
{
    CodePage = codePage;
    Escape.clear();
    Character.clear();
    CharacterSize = 0;
    PreviousCR = false;
    Pasting = false;
}

void LOCAL_LINE_EDITOR::Clear()
{
    Line.clear();
    Cursor = 0;
    Browsing = false;
    Draft.clear();
    SearchPrefix.clear();
    // Submit 后保留 CR 状态，避免同一粘贴块中的 CRLF 被发送两次
}

std::wstring LOCAL_LINE_EDITOR::Suggestion() const
{
    if (Line.empty() || Cursor != Line.size() || Pasting)
        return {};
    for (auto i = History.Entries().rbegin(); i != History.Entries().rend(); ++i)
        if (i->size() > Line.size() && i->compare(0, Line.size(), Line) == 0 &&
            !EncodeTerminalText(*i, CodePage).empty())
            return i->substr(Line.size());
    return {};
}

void LOCAL_LINE_EDITOR::AcceptSuggestion()
{
    auto suggestion = Suggestion();
    if (!suggestion.empty())
        Insert(suggestion);
}

void LOCAL_LINE_EDITOR::Browse(bool older)
{
    const auto &entries = History.Entries();
    if (!Browsing)
    {
        if (!older)
            return;
        Draft = Line;
        SearchPrefix = Line.substr(0, Cursor);
        HistoryIndex = entries.size();
        Browsing = true;
    }
    size_t index = (std::min)(HistoryIndex, entries.size());
    while (older ? index > 0 : index < entries.size())
    {
        index = older ? index - 1 : index + 1;
        if (index == entries.size())
            break;
        if (entries[index].compare(0, SearchPrefix.size(), SearchPrefix) == 0 &&
            !EncodeTerminalText(entries[index], CodePage).empty())
        {
            HistoryIndex = index;
            Line = entries[index];
            Cursor = Line.size();
            return;
        }
    }
    if (!older)
    {
        Line = Draft;
        Cursor = Line.size();
        Browsing = false;
    }
}

INPUT_ACTION LOCAL_LINE_EDITOR::Feed(uint8_t byte, UINT codePage)
{
    CodePage = codePage;
    // 新 Esc 可重新开始转义，避免不完整的粘贴内容吞掉粘贴结束标记
    // 裸 Esc 与 Alt 组合键共用前缀；只限制裸前缀的等待时间，不给已开始的 CSI/SS3 或粘贴计时
    if (!Escape.empty() && (byte == '\033' || (!Pasting &&
        (byte < 32 || byte == 127 || (Escape.size() == 1 && GetTickCount64() - EscapeStarted >= 100)))))
        Escape.clear();
    if (!Escape.empty())
    {
        Escape += static_cast<char>(byte);
        if (Escape == "\033O" || (!EscapeFinished(Escape) && Escape.size() < 64))
            return INPUT_ACTION::None;
        auto key = Escape;
        Escape.clear();
        if (key == "\033[200~")
        {
            Pasting = true;
            return INPUT_ACTION::None;
        }
        if (key == "\033[201~")
        {
            Pasting = false;
            return INPUT_ACTION::None;
        }
        if (Pasting)
            return INPUT_ACTION::None;
        if (key == "\033[A" || key == "\033OA")
            Browse(true);
        else if (key == "\033[B" || key == "\033OB")
            Browse(false);
        else if (key == "\033[D" || key == "\033OD")
            Cursor = PreviousCharacter(Line, Cursor);
        else if (key == "\033[C" || key == "\033OC")
        {
            if (Cursor == Line.size())
                AcceptSuggestion();
            else
                Cursor = NextCharacter(Line, Cursor);
        }
        else if (key == "\033[H" || key == "\033OH" || key == "\033[1~" || key == "\033[7~")
            Cursor = 0;
        else if (key == "\033[F" || key == "\033OF" || key == "\033[4~" || key == "\033[8~")
            Cursor = Line.size();
        else if (key == "\033[3~" && Cursor < Line.size())
        {
            Edit();
            Line.erase(Cursor, NextCharacter(Line, Cursor) - Cursor);
        }
        return INPUT_ACTION::None;
    }
    if (Character.empty() && byte == '\033')
    {
        Escape = "\033";
        EscapeStarted = GetTickCount64();
        PreviousCR = false;
        return INPUT_ACTION::None;
    }
    if (byte == '\r' || byte == '\n')
    {
        bool duplicate = byte == '\n' && PreviousCR;
        PreviousCR = byte == '\r';
        if (!Character.empty())
        {
            Character.clear();
            return INPUT_ACTION::Rejected;
        }
        if (duplicate)
            return INPUT_ACTION::None;
        return Pasting ? Insert(L" ") : INPUT_ACTION::Submit;
    }
    PreviousCR = false;
    if (byte < 32 || byte == 127)
    {
        Character.clear();
        if (Pasting)
            return byte == '\t' ? Insert(L" ") : INPUT_ACTION::None;
        switch (byte)
        {
        case 3:
            Clear();
            return INPUT_ACTION::Interrupt;
        case 11:
            return INPUT_ACTION::OpenSettings;
        case 8:
        case 127:
            if (Cursor > 0)
            {
                Edit();
                auto previous = PreviousCharacter(Line, Cursor);
                Line.erase(previous, Cursor - previous);
                Cursor = previous;
            }
            break;
        case 5:
            Cursor = Line.size();
            break;
        case 2:
            Cursor = PreviousCharacter(Line, Cursor);
            break;
        case 9:
            AcceptSuggestion();
            break;
        }
        return INPUT_ACTION::None;
    }
    if (Character.empty())
        CharacterSize = EncodedCharacterSize(byte, codePage);
    Character += static_cast<char>(byte);
    if (Character.size() < CharacterSize)
        return INPUT_ACTION::None;
    std::wstring text;
    bool valid = DecodeText(Character, codePage, text);
    Character.clear();
    return valid ? Insert(text) : INPUT_ACTION::Rejected;
}

static int CharacterWidth(const std::wstring &text, size_t position)
{
    unsigned int ch = text[position];
    if (NextCharacter(text, position) == position + 2)
        return 2;
    WORD type = 0;
    GetStringTypeW(CT_CTYPE3, &text[position], 1, &type);
    if (type & C3_NONSPACING)
        return 0;
    if ((type & C3_FULLWIDTH) || (ch >= 0x2e80 && ch <= 0xa4cf) || (ch >= 0xac00 && ch <= 0xd7a3) ||
        (ch >= 0xf900 && ch <= 0xfaff))
        return 2;
    return 1;
}

std::string LOCAL_LINE_EDITOR::Render(int row, int columns, UINT codePage) const
{
    int available = (std::max)(1, columns - 4);
    size_t begin = Cursor;
    int cursorWidth = 0;
    while (begin > 0)
    {
        auto previous = PreviousCharacter(Line, begin);
        int width = CharacterWidth(Line, previous);
        if (cursorWidth + width >= available)
            break;
        begin = previous;
        cursorWidth += width;
    }
    size_t end = begin;
    int width = 0;
    while (end < Line.size() && width + CharacterWidth(Line, end) <= available)
    {
        width += CharacterWidth(Line, end);
        end = NextCharacter(Line, end);
    }
    auto suggestion = Suggestion();
    size_t suggestionEnd = 0;
    if (end == Line.size())
        while (suggestionEnd < suggestion.size() && width + CharacterWidth(suggestion, suggestionEnd) <= available)
        {
            width += CharacterWidth(suggestion, suggestionEnd);
            suggestionEnd = NextCharacter(suggestion, suggestionEnd);
        }
    std::string output = "\033[" + std::to_string(row) + ";1H\033[0m\033[2K\033[96m> \033[39m";
    auto typed = EncodeTerminalText(Line.substr(begin, end - begin), codePage);
    output.append(typed.begin(), typed.end());
    auto proposed = EncodeTerminalText(suggestion.substr(0, suggestionEnd), codePage);
    output += "\033[90m";
    output.append(proposed.begin(), proposed.end());
    output += "\033[0m\033[" + std::to_string(row) + ";" + std::to_string(3 + cursorWidth) + "H";
    return output;
}
