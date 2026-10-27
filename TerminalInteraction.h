#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

std::string LocalTimestamp();
std::vector<uint8_t> EncodeTerminalText(const std::wstring &text, UINT codePage);
DWORD CommandHistoryPath(std::wstring &path);

class COMMAND_HISTORY
{
  public:
    static const size_t MaxEntries = 500;
    static const size_t MaxCommandLength = 4096;

    DWORD Load(const std::wstring &path);
    DWORD Remember(const std::wstring &command);
    const std::vector<std::wstring> &Entries() const
    {
        return Commands;
    }

  private:
    std::wstring Path;
    std::vector<std::wstring> Commands;
    std::vector<std::wstring> Unsaved;
};

// 只匹配发送后的第一行；失配时归还暂存前缀，不搜索后续日志
class COMMAND_ECHO_FILTER
{
  public:
    void Expect(const std::vector<uint8_t> &command);
    std::vector<uint8_t> Feed(const uint8_t *&data, size_t &size);
    std::vector<uint8_t> Flush();
    bool Waiting() const { return !Expected.empty() || SkipLF; }

  private:
    std::vector<uint8_t> Expected;
    size_t Matched = 0;
    bool SkipLF = false;
};

// 在插入显示用转义序列前，先收齐串口中被分包的字符和 ANSI 序列
class RECEIVE_FORMATTER
{
  public:
    std::vector<uint8_t> Feed(const uint8_t *data, size_t size, const std::string &time, bool timestamps, bool logMode,
                              UINT codePage);
    std::vector<uint8_t> Flush();
    void BeginLine();
    void Reset();

  private:
    bool LineStart = true;
    bool PreviousCR = false;
    std::string LineTime;
    std::string Escape;
    std::string Character;
    size_t CharacterSize = 0;
};

enum class INPUT_ACTION
{
    None,
    Submit,
    Interrupt,
    EndOfFile,
    Rejected
};

class LOCAL_LINE_EDITOR
{
  public:
    explicit LOCAL_LINE_EDITOR(const COMMAND_HISTORY &history) : History(history) {}
    INPUT_ACTION Feed(uint8_t byte, UINT codePage);
    const std::wstring &Text() const
    {
        return Line;
    }
    size_t Position() const
    {
        return Cursor;
    }
    std::wstring Suggestion() const;
    void Clear();
    void ResetDecoder(UINT codePage);
    std::string Render(int row, int columns, UINT codePage) const;

  private:
    void Edit();
    void Browse(bool older);
    void AcceptSuggestion();
    INPUT_ACTION Insert(const std::wstring &text);
    const COMMAND_HISTORY &History;
    std::wstring Line;
    size_t Cursor = 0;
    bool Browsing = false;
    size_t HistoryIndex = 0;
    std::wstring Draft;
    std::wstring SearchPrefix;
    std::string Escape;
    std::string Character;
    size_t CharacterSize = 0;
    bool PreviousCR = false;
    bool Pasting = false;
    UINT CodePage = CP_UTF8;
};
