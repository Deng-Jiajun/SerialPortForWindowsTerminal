#define wmain SerialProgramMainForTest
#include "../SerialForWindowsTerminal.cpp"
#undef wmain
#include <fstream>

static int Failures = 0;
static int Checks = 0;

static void Check(bool condition, const char* name)
{
    ++Checks;
    if (!condition) { ++Failures; std::cerr << "FAIL: " << name << '\n'; }
}

static std::string Format(RECEIVE_FORMATTER& formatter, const std::string& input,
    const char* time = "[10:00:00.001] ", bool timestamps = true, bool logMode = true, UINT codePage = CP_UTF8)
{
    auto bytes = formatter.Feed(reinterpret_cast<const uint8_t*>(input.data()), input.size(), time, timestamps, logMode, codePage);
    return std::string(bytes.begin(), bytes.end());
}

static int Type(LOCAL_LINE_EDITOR& editor, const std::string& input, UINT codePage = CP_UTF8)
{
    int submitted = 0;
    for (auto byte : input)
        if (editor.Feed(static_cast<uint8_t>(byte), codePage) == INPUT_ACTION::Submit) ++submitted;
    return submitted;
}

static std::string Bytes(const std::wstring& text, UINT codePage = CP_UTF8)
{
    auto bytes = EncodeTerminalText(text, codePage);
    return std::string(bytes.begin(), bytes.end());
}

static std::string FilterEcho(COMMAND_ECHO_FILTER& filter, const std::string& input)
{
    auto data = reinterpret_cast<const uint8_t*>(input.data());
    auto size = input.size();
    auto pending = filter.Feed(data, size);
    std::string output(pending.begin(), pending.end());
    output.append(reinterpret_cast<const char*>(data), size);
    return output;
}

static void TestEchoFilter()
{
    const std::string sample = "pwd\r\n/customer/zctrl\r\npwd\r\n/customer/zctrl # ";
    const std::string expected = "/customer/zctrl\r\npwd\r\n/customer/zctrl # ";
    for (size_t split = 0; split <= sample.size(); ++split)
    {
        COMMAND_ECHO_FILTER filter;
        filter.Expect({'p', 'w', 'd'});
        auto output = FilterEcho(filter, sample.substr(0, split));
        output += FilterEcho(filter, sample.substr(split));
        Check(output == expected, "hide only first complete echo at every packet boundary");
    }
    COMMAND_ECHO_FILTER filter;
    Check(FilterEcho(filter, sample) == sample, "unarmed filter preserves all received bytes");
    for (auto ending : {"\r", "\n", "\r\n"})
    {
        filter.Expect({'p', 'w', 'd'});
        Check(FilterEcho(filter, std::string("pwd") + ending + "/\r\n") == "/\r\n", "hide echo with CR LF or CRLF");
    }
    filter.Expect({'p', 'w', 'd'});
    Check(FilterEcho(filter, "pw").empty(), "hold only a matching prefix");
    Check(FilterEcho(filter, "X\r\npwd\r\n") == "pwX\r\npwd\r\n", "mismatch restores prefix and does not filter later equal lines");
    filter.Expect({'p', 'w', 'd'});
    Check(FilterEcho(filter, "pwd: error\r\n") == "pwd: error\r\n", "command prefix in real output is not an echo");
    filter.Expect({'p', 'w', 'd'});
    Check(FilterEcho(filter, "pw\r\n") == "pw\r\n", "shorter received line is preserved");
    filter.Expect({'p', 'w', 'd'});
    FilterEcho(filter, "pw");
    auto pending = filter.Flush();
    Check(std::string(pending.begin(), pending.end()) == "pw" && !filter.Waiting(), "flush restores unfinished echo and disarms filter");
    Check(FilterEcho(filter, "d\r\n") == "d\r\n", "late echo after flush is not suppressed");
    filter.Expect({});
    Check(FilterEcho(filter, "\r\n") == "\r\n", "empty command does not remove received blank lines");
    for (auto codePage : {static_cast<UINT>(CP_UTF8), 936u})
    {
        auto command = EncodeTerminalText(L"echo 中文", codePage);
        filter.Expect(command);
        std::string output;
        for (auto byte : command) output += FilterEcho(filter, std::string(1, static_cast<char>(byte)));
        output += FilterEcho(filter, "\r");
        output += FilterEcho(filter, "\n" + Bytes(L"中文", codePage) + "\r\n");
        Check(output == Bytes(L"中文", codePage) + "\r\n", "UTF-8 and GBK echoes match across individual bytes");
    }
}

static void TestReceive()
{
    const std::string sample = "\033[31m温度 ERROR\033[0m\r\nnext\n\nlast";
    const std::string expected = "\033[31m[10:00:00.001] 温度 ERROR\033[0m\r\n[10:00:00.001] next\r\n\r\n[10:00:00.001] last";
    for (size_t split = 0; split <= sample.size(); ++split)
    {
        RECEIVE_FORMATTER state;
        auto output = Format(state, sample.substr(0, split));
        output += Format(state, sample.substr(split));
        Check(output == expected, "timestamp independent of packet split (UTF-8, SGR, CRLF)");
    }
    RECEIVE_FORMATTER state;
    Check(Format(state, "a", "[first] ") == "[first] a", "unterminated line appears immediately");
    Check(Format(state, "b\r", "[second] ") == "b\r\n", "no timestamp on continuation");
    Check(Format(state, "\nc\r\rd", "[third] ") == "[third] c\r\n\r\n[third] d", "CRLF across reads and lone CR");
    state.Reset();
    auto chinese = Bytes(L"中文");
    Check(Format(state, chinese.substr(0, 1), "[first] ").empty(), "hold partial character before redraw");
    Check(Format(state, chinese.substr(1), "[later] ") == "[first] " + chinese, "first byte timestamp retained");
    state.Reset();
    Format(state, chinese.substr(0, 1), "[first] ");
    state.BeginLine();
    Check(Format(state, chinese.substr(1), "[later] ") == "[first] " + chinese, "sending a record does not discard partial RX character");
    state.Reset();
    auto gbk = Bytes(L"中文", 936);
    std::string combined;
    for (auto byte : gbk) combined += Format(state, std::string(1, byte), "[t] ", true, true, 936);
    Check(combined == "[t] " + gbk, "GBK byte split");
    state.Reset();
    Check(Format(state, "\033[2J\033[1;1Hhello\033]0;title\a\r\n") == "[10:00:00.001] hello\r\n", "local log mode excludes cursor and OSC commands");
    state.Reset();
    Check(Format(state, "\033[2Jhello", "[t] ", false, false) == "\033[2Jhello", "formatter can retain ANSI outside log mode");
    state.Reset();
    Check(Format(state, "abc\r\ndef", "[t] ", false) == "abc\r\ndef", "timestamps off");

    SERIAL_CONFIG cfg = {};
    cfg.HighlightRules = {{L"ERROR", HIGHLIGHT_COLOR_RED}, {L"WARN", HIGHLIGHT_COLOR_YELLOW}};
    HIGHLIGHT_OUTPUT_STATE highlight;
    ResetHighlightOutputState(highlight, cfg);
    state.Reset();
    std::string highlighted;
    for (const auto& part : {"ER", "ROR\r", "\nWARN x"})
    {
        auto formatted = Format(state, part, "[t] ");
        auto output = BuildHighlightedOutput(highlight, reinterpret_cast<const uint8_t*>(formatted.data()), formatted.size(), false);
        highlighted.append(output.begin(), output.end());
    }
    Check(highlighted == "[t] \033[91mERROR\033[39m\r\n[t] \033[93mWARN\033[39m x", "existing keyword highlighting with timestamps");
}

static void TestHistoryAndInput(const std::wstring& path)
{
    COMMAND_HISTORY history;
    Check(history.Load(path) == 0, "missing history is empty");
    Check(history.Remember(L"make clean && make -j") == 0, "save command");
    Check(history.Remember(L"ls") == 0, "save second command");
    Check(history.Remember(L"make clean && make -j") == 0 && history.Entries().size() == 2, "deduplicate to most recent");
    Check(history.Remember(L"echo 中文") == 0, "save UTF-8 history");
    COMMAND_HISTORY restarted;
    Check(restarted.Load(path) == 0 && restarted.Entries() == history.Entries(), "history restored after restart");
    Check(restarted.Remember(L" local-only") == 0 && restarted.Entries().size() == 3, "leading space excludes history");
    Check(restarted.Remember(L"echo one\necho two") == ERROR_INVALID_DATA, "reject multiline persisted record");

    LOCAL_LINE_EDITOR editor(history);
    Type(editor, "ma");
    Check(editor.Text() == L"ma" && editor.Suggestion() == L"ke clean && make -j", "prefix produces grey suggestion");
    Check(editor.Render(24, 80, CP_UTF8).find("\033[90mke clean && make -j") != std::string::npos, "suggestion rendered in grey");
    Type(editor, "\033[C");
    Check(editor.Text() == L"make clean && make -j" && editor.Suggestion().empty(), "right arrow accepts without submitting");
    Type(editor, "\033[H\033[3~M");
    Check(editor.Text() == L"Make clean && make -j" && editor.Position() == 1, "home delete insertion");
    Type(editor, "\033[F\177");
    Check(editor.Text() == L"Make clean && make -", "end and backspace");
    editor.Clear();
    Type(editor, "ma\033[A");
    Check(editor.Text() == L"make clean && make -j", "up searches matching prefix");
    Type(editor, "\033[B");
    Check(editor.Text() == L"ma", "down restores unfinished draft");
    editor.Clear();
    Type(editor, Bytes(L"中😀文"));
    Type(editor, "\033[D\177");
    Check(editor.Text() == L"中文" && editor.Position() == 1, "delete complete UTF-16 surrogate pair");
    Type(editor, "\177");
    Check(editor.Text() == L"文", "backspace complete Chinese character");
    editor.Clear();
    Check(Type(editor, "\033[200~echo one\r\necho two\033[201~") == 0, "bracketed paste never submits");
    Check(editor.Text() == L"echo one echo two", "paste line breaks become spaces");
    Check(editor.Feed('\r', CP_UTF8) == INPUT_ACTION::Submit, "Enter submits draft");
    editor.Clear();
    Check(editor.Feed('\n', CP_UTF8) == INPUT_ACTION::None, "CRLF not double submitted after clearing");
    Type(editor, "echo");
    for (auto key : {3, 4, 6, 21, 23})
        Check(editor.Feed(static_cast<uint8_t>(key), CP_UTF8) == INPUT_ACTION::None && editor.Text() == L"echo",
              "removed local control shortcuts preserve the draft");
    Check(editor.Feed(11, CP_UTF8) == INPUT_ACTION::OpenSettings && editor.Text() == L"echo",
          "Ctrl+K opens settings without deleting the draft");
    Type(editor, "\033[200~");
    Check(editor.Feed(11, CP_UTF8) == INPUT_ACTION::None, "pasted Ctrl+K does not open settings");
    Type(editor, "\033[201~");
    editor.Clear();
    Type(editor, std::string(4096, 'x'));
    Check(editor.Feed('y', CP_UTF8) == INPUT_ACTION::Rejected && editor.Text().size() == 4096, "input size bound");
    Check(editor.Render(12, 25, CP_UTF8).find("\033[12;23H") != std::string::npos, "long input scrolls horizontally with visible cursor");
    editor.Clear();
    editor.ResetDecoder(936);
    Type(editor, Bytes(L"中文", 936), 936);
    Type(editor, "\177", 936);
    Check(editor.Text() == L"中", "GBK input and backspace");

    COMMAND_HISTORY otherWindow;
    Check(otherWindow.Load(path) == 0, "second window loads shared history");
    Check(history.Remember(L"first-window") == 0 && otherWindow.Remember(L"second-window") == 0, "sequential window saves merge");
    COMMAND_HISTORY merged;
    Check(merged.Load(path) == 0 && merged.Entries().size() == 5, "no commands lost between windows");
    DWORD firstError = 0, secondError = 0;
    std::thread first([&]() { for (int i = 0; i < 25; ++i) firstError |= history.Remember(L"left-" + std::to_wstring(i)); });
    std::thread second([&]() { for (int i = 0; i < 25; ++i) secondError |= otherWindow.Remember(L"right-" + std::to_wstring(i)); });
    first.join(); second.join();
    DWORD mergedError = merged.Load(path);
    if (firstError || secondError || mergedError || merged.Entries().size() != 55)
        std::cerr << "concurrent results: " << firstError << ", " << secondError << ", " << mergedError << ", entries=" << merged.Entries().size() << '\n';
    Check(firstError == 0 && secondError == 0 && mergedError == 0 && merged.Entries().size() == 55, "concurrent saves retain both writers");

    auto boundedPath = path + L".bounded";
    {
        std::ofstream file(boundedPath, std::ios::binary);
        for (int i = 0; i < 510; ++i) file << "command-" << i << "\n";
    }
    COMMAND_HISTORY bounded;
    Check(bounded.Load(boundedPath) == 0 && bounded.Entries().size() == 500 && bounded.Entries().front() == L"command-10", "retain latest 500 on load");
    Check(bounded.Remember(L"last") == 0 && bounded.Entries().size() == 500 && bounded.Entries().front() == L"command-11", "retain latest 500 on save");
    auto damagedPath = path + L".damaged";
    { std::ofstream file(damagedPath, std::ios::binary); file << "\xff\xfe\x80"; }
    COMMAND_HISTORY damaged;
    Check(damaged.Load(damagedPath) == ERROR_NO_UNICODE_TRANSLATION, "invalid history is reported");
    Check(damaged.Remember(L"new-command") == ERROR_NO_UNICODE_TRANSLATION, "invalid history is not overwritten");
    { std::ifstream file(damagedPath, std::ios::binary); std::string contents((std::istreambuf_iterator<char>(file)), {}); Check(contents == "\xff\xfe\x80", "damaged original preserved"); }
}

// 用字节管道代替串口，经过生产输入处理和异步写队列检查实际发送内容
struct TEST_BYTE_PIPE
{
    HANDLE Server = INVALID_HANDLE_VALUE;
    HANDLE Client = INVALID_HANDLE_VALUE;

    TEST_BYTE_PIPE()
    {
        static unsigned next = 0;
        auto name = L"\\\\.\\pipe\\SerialInteractionTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++next);
        Server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 8192, 8192, 0, nullptr);
        Client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (Server == INVALID_HANDLE_VALUE || Client == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot create test byte pipe");
    }

    ~TEST_BYTE_PIPE()
    {
        if (Server != INVALID_HANDLE_VALUE) CloseHandle(Server);
        if (Client != INVALID_HANDLE_VALUE) CloseHandle(Client);
    }

    HANDLE TakeServer()
    {
        auto handle = Server;
        Server = INVALID_HANDLE_VALUE;
        return handle;
    }
};

static void TestLocalSubmission(const std::wstring& historyPath, const std::string& keys,
    const std::string& expected, const char* name)
{
    TEST_BYTE_PIPE inputPipe, serialPipe, outputPipe;
    boost::asio::io_service io;
    boost::asio::windows::stream_handle input(io, inputPipe.TakeServer());
    boost::asio::windows::stream_handle output(io, outputPipe.TakeServer());
    boost::asio::serial_port serial(io);
    serial.assign(serialPipe.TakeServer());
    SERIAL_WRITE_QUEUE serialQueue(serial);
    CONSOLE_WRITE_QUEUE outputQueue(output);
    SERIAL_CONFIG cfg = {};
    cfg.EchoMode = 1;
    COMMAND_HISTORY history;
    history.Load(historyPath);
    LOCAL_LINE_EDITOR editor(history);
    TERMINAL_DISPLAY display(io, outputQueue, cfg, editor);
    AUTOMATION_SERVER automation(io, cfg, serialQueue, display);
    std::vector<uint8_t> inputBuffer(1024);
    DWORD written = 0;
    if (!WriteFile(inputPipe.Client, keys.data(), static_cast<DWORD>(keys.size()), &written, nullptr))
        throw std::runtime_error("cannot write test input");
    DoConsoleToSerial(input, serialQueue, inputBuffer, cfg, display, editor, history, automation, []() {});
    io.run_for(std::chrono::milliseconds(200));
    DWORD available = 0;
    if (!PeekNamedPipe(serialPipe.Client, nullptr, 0, nullptr, &available, nullptr))
        throw std::runtime_error("cannot inspect test serial output");
    std::string sent(available, '\0');
    DWORD received = 0;
    if (available && !ReadFile(serialPipe.Client, &sent[0], available, &received, nullptr))
        throw std::runtime_error("cannot read test serial output");
    sent.resize(received);
    Check(sent == expected, name);
}

static void TestSettingsShortcut(bool local)
{
    TEST_BYTE_PIPE inputPipe, serialPipe, outputPipe;
    boost::asio::io_service io;
    boost::asio::windows::stream_handle input(io, inputPipe.TakeServer());
    boost::asio::windows::stream_handle output(io, outputPipe.TakeServer());
    boost::asio::serial_port serial(io);
    serial.assign(serialPipe.TakeServer());
    SERIAL_WRITE_QUEUE serialQueue(serial);
    CONSOLE_WRITE_QUEUE outputQueue(output);
    SERIAL_CONFIG cfg = {};
    cfg.EchoMode = local ? 1 : 0;
    COMMAND_HISTORY history;
    LOCAL_LINE_EDITOR editor(history);
    Type(editor, "draft");
    TERMINAL_DISPLAY display(io, outputQueue, cfg, editor);
    AUTOMATION_SERVER automation(io, cfg, serialQueue, display);
    const std::string controls = "\x01\x03\x04\x06\x15\x17";
    const std::string keys = "\x0b" + controls + "x";
    DWORD written = 0;
    if (!WriteFile(inputPipe.Client, keys.data(), static_cast<DWORD>(keys.size()), &written, nullptr))
        throw std::runtime_error("cannot write settings shortcut test input");
    int opened = 0;
    std::wstring openingDraft;
    std::vector<uint8_t> buffer(1024);
    DoConsoleToSerial(input, serialQueue, buffer, cfg, display, editor, history, automation, [&]() {
        ++opened;
        openingDraft = editor.Text();
    });
    io.run_for(std::chrono::milliseconds(200));
    DWORD available = 0, read = 0;
    if (!PeekNamedPipe(serialPipe.Client, nullptr, 0, nullptr, &available, nullptr))
        throw std::runtime_error("cannot inspect settings shortcut serial bytes");
    std::string sent(available, '\0');
    if (available) ReadFile(serialPipe.Client, &sent[0], available, &read, nullptr);
    sent.resize(read);
    Check(opened == 1 && openingDraft == L"draft", "Ctrl+K opens settings once and preserves the existing draft");
    Check(sent == (local ? "" : controls + "x"), "only Ctrl+K is intercepted in raw mode; removed local shortcuts send nothing");
    Check(editor.Text() == (local ? L"draftx" : L"draft"), "old Ctrl+A prefix no longer consumes the next normal character");
    Check(cfg.EchoMode == (local ? 1u : 0u) && cfg.EncodingFormat == 0, "old Ctrl+A chords do not change settings");
}

static void TestEchoDisplay(bool local, bool external, bool timeout)
{
    TEST_BYTE_PIPE outputPipe;
    boost::asio::io_service io;
    boost::asio::windows::stream_handle output(io, outputPipe.TakeServer());
    CONSOLE_WRITE_QUEUE queue(output);
    SERIAL_CONFIG cfg = {};
    cfg.EchoMode = local ? 1 : 0;
    cfg.Timestamps = 1;
    COMMAND_HISTORY history;
    LOCAL_LINE_EDITOR editor(history);
    TERMINAL_DISPLAY display(io, queue, cfg, editor);
    display.Sent({'p', 'w', 'd'}, external);
    const std::string prefix = "pw";
    display.Receive(reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size(), "[first] ");
    if (timeout)
    {
        io.run_for(std::chrono::milliseconds(1100));
        io.restart();
    }
    const std::string rest = "d\r\n/\r\npwd\r\n/ # ";
    display.Receive(reinterpret_cast<const uint8_t*>(rest.data()), rest.size(), "[later] ");
    io.run_for(std::chrono::milliseconds(100));
    DWORD available = 0, read = 0;
    if (!PeekNamedPipe(outputPipe.Client, nullptr, 0, nullptr, &available, nullptr))
        throw std::runtime_error("cannot inspect echo display output");
    std::string text(available, '\0');
    if (available) ReadFile(outputPipe.Client, &text[0], available, &read, nullptr);
    text.resize(read);
    Check(text.find(external ? "\033[96m[TX External] > pwd" : "\033[96m> pwd") != std::string::npos,
          "blue timestamped transmission record is retained");
    const bool suppressed = local && !external && !timeout;
    Check((text.find("[first] pwd\r\n") == std::string::npos) == suppressed,
          "echo display filtering applies only to timely local manual input");
    Check(text.find("[later] /\r\n[later] pwd\r\n[later] / # ") != std::string::npos,
          "result later equal line prompt and receive timestamps are retained");
}

static void TestErrorEncoding()
{
    wchar_t systemMessage[1024] = {};
    auto length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                 ERROR_OPERATION_ABORTED, 0, systemMessage, _countof(systemMessage), nullptr);
    Check(length != 0, "Windows error description is available for the encoding check");
    if (!length) return;
    while (length && (systemMessage[length - 1] == L'\r' || systemMessage[length - 1] == L'\n')) --length;
    std::wstring expected(systemMessage, length);
    for (UINT codePage : {static_cast<UINT>(CP_UTF8), 936U})
    {
        auto text = ErrorMessage({ERROR_OPERATION_ABORTED, boost::system::system_category()}, codePage);
        Check(text.find("995: ") == 0, "error description retains its numeric Windows code");
        auto detail = text.substr((std::min)(size_t(5), text.size()));
        auto count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, detail.data(), static_cast<int>(detail.size()), nullptr, 0);
        std::wstring decoded(count, L'\0');
        if (count) MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, detail.data(), static_cast<int>(detail.size()), &decoded[0], count);
        Check(decoded == expected, codePage == CP_UTF8 ? "Windows error text decodes correctly as UTF-8" :
                                                       "Windows error text decodes correctly as GBK");
    }
}

static void TestSerialTermination(bool cancelled, bool stopping)
{
    TEST_BYTE_PIPE serialPipe, outputPipe;
    boost::asio::io_service io;
    boost::asio::serial_port serial(io);
    serial.assign(serialPipe.TakeServer());
    boost::asio::windows::stream_handle output(io, outputPipe.TakeServer());
    SERIAL_WRITE_QUEUE serialQueue(serial);
    CONSOLE_WRITE_QUEUE outputQueue(output);
    SERIAL_CONFIG cfg = {};
    COMMAND_HISTORY history;
    LOCAL_LINE_EDITOR editor(history);
    TERMINAL_DISPLAY display(io, outputQueue, cfg, editor);
    AUTOMATION_SERVER automation(io, cfg, serialQueue, display);
    std::vector<uint8_t> buffer(1024);
    DoSerialToConsole(serial, buffer, display, automation);
    if (stopping)
        display.Cancel();
    if (cancelled)
        serial.cancel();
    else
    {
        CloseHandle(serialPipe.Client);
        serialPipe.Client = INVALID_HANDLE_VALUE;
    }
    io.run_for(std::chrono::seconds(2));

    DWORD available = 0;
    Check(PeekNamedPipe(outputPipe.Client, nullptr, 0, nullptr, &available, nullptr) != FALSE,
          "serial termination output remains readable");
    std::string text(available, '\0');
    DWORD read = 0;
    if (available) ReadFile(outputPipe.Client, &text[0], available, &read, nullptr);
    text.resize(read);
    if (stopping)
    {
        Check(!display.ExitError(), "intentional read cancellation is not a disconnect failure");
        Check(text.empty(), "intentional cancellation emits no disconnect warning");
    }
    else
    {
        Check(display.ExitError().value() != 0, "serial disconnect preserves a nonzero exit error");
        if (cancelled)
            Check(display.ExitError() == boost::asio::error::operation_aborted,
                  "unexpected driver cancellation remains an error while the session is active");
        Check(io.stopped(), "serial disconnect completes output and stops the event loop");
        Check(text.find("[Error] Serial receive:") != std::string::npos &&
              text.find("reopen this terminal profile") != std::string::npos,
              "serial disconnect displays both the cause and the recovery action");
    }
}

static void TestCancelledWrites()
{
    TEST_BYTE_PIPE serialPipe;
    boost::asio::io_service io;
    boost::asio::serial_port serial(io);
    serial.assign(serialPipe.TakeServer());
    SERIAL_WRITE_QUEUE queue(serial);
    bool nextStarted = false;
    bool completed = false;
    // 大于管道缓冲区的写入保持挂起，检查退出时不会再发送排队命令
    queue.Enqueue(std::vector<uint8_t>(32768, 'x'), {}, [&](const boost::system::error_code&) { completed = true; });
    queue.Enqueue({'n', 'e', 'x', 't'}, [&]() { nextStarted = true; return true; });
    queue.Stop();
    serial.cancel();
    io.run_for(std::chrono::seconds(2));
    Check(io.stopped(), "cancelled write releases the event loop");
    Check(!nextStarted, "shutdown discards queued serial commands");
    Check(!completed, "shutdown does not run normal write completion actions");
}

static int Preview(const std::wstring& path, bool runtimeSettings = false, HWND settingsParent = nullptr)
{
    SetConsoleTitleW(L"Serial terminal - timestamp and history verification");
    DWORD mode = 0;
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
    boost::asio::io_service io;
    boost::asio::windows::stream_handle output(io,
        CreateFileW(L"CONOUT$", FILE_GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    CONSOLE_WRITE_QUEUE queue(output);
    SERIAL_CONFIG cfg = {};
    cfg.EchoMode = 1;
    cfg.Timestamps = 1;
    cfg.HighlightRules = {{L"ERROR", HIGHLIGHT_COLOR_RED}, {L"WARN", HIGHLIGHT_COLOR_YELLOW}};
    COMMAND_HISTORY history;
    history.Load(path);
    history.Remember(L"make clean && make -j");
    LOCAL_LINE_EDITOR editor(history);
    TERMINAL_DISPLAY display(io, queue, cfg, editor);
    hInstance = GetModuleHandleW(nullptr);
    TERMINAL_SETTINGS_DIALOG settings(io, cfg, display, editor);
    display.Start();
    Type(editor, "ma");
    display.Redraw();
    auto send = [&](const std::string& data) { display.Receive(reinterpret_cast<const uint8_t*>(data.data()), data.size(), LocalTimestamp()); };
    display.Sent(EncodeTerminalText(L"pwd", CP_UTF8), false);
    send("pwd\r\n/customer/zctrl\r\n/customer/zctrl # ");
    send("[DEBUG] connected to test device\r\n");
    send("[WARN] reconnecting\r\n");
    send("[ERROR] connection timed out\r\n");
    send("[DEBUG] 中文日志，输入草稿与历史建议保持显示\r\n");
    boost::asio::steady_timer ticker(io);
    TEST_BYTE_PIPE serialPipe;
    boost::asio::serial_port serial(io);
    serial.assign(serialPipe.TakeServer());
    SERIAL_WRITE_QUEUE serialQueue(serial);
    AUTOMATION_SERVER automation(io, cfg, serialQueue, display);
    boost::asio::windows::stream_handle input(io,
        CreateFileW(L"CONIN$", FILE_GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode);
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), (mode | ENABLE_VIRTUAL_TERMINAL_INPUT) &
        ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT));
    std::vector<uint8_t> inputBuffer(1024);
    DoConsoleToSerial(input, serialQueue, inputBuffer, cfg, display, editor, history, automation,
        [&]() { settings.Open(); });
    if (runtimeSettings) settings.Open(settingsParent);
    int count = 0;
    std::function<void()> tick = [&]()
    {
        ticker.expires_after(std::chrono::milliseconds(700));
        ticker.async_wait([&](const boost::system::error_code& ec)
        {
            if (ec) return;
            ++count;
            if (count % 5 == 0) send("[WARN] 中文日志：输入草稿与灰色建议保持不变\r\n");
            else if (count % 7 == 0) send("[ERROR] simulated timeout\r\n");
            else send("[DEBUG] background log " + std::to_string(count) + "\r\n");
            if (count == 4) display.Sent(EncodeTerminalText(L"echo hello", CP_UTF8), false);
            if (count < (runtimeSettings ? 600 : 120) && (!runtimeSettings || settings.IsOpen())) tick();
            else display.Stop();
        });
    };
    tick();
    io.run();
    display.Cancel();
    settings.Stop();
    serialQueue.Stop();
    queue.Stop();
    input.cancel();
    output.cancel();
    serial.cancel();
    ticker.cancel();
    io.restart();
    io.run();
    if (runtimeSettings)
    {
        HKEY saved = nullptr;
        auto settingsSaved = RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\SerialForWindowsTerminal", 0,
                                           KEY_READ, &saved) == ERROR_SUCCESS;
        if (saved) RegCloseKey(saved);
        std::ofstream result(path + L".result.txt", std::ios::binary);
        result << "encoding=" << cfg.EncodingFormat << "\nlocal=" << cfg.EchoMode
               << "\ntimestamps=" << cfg.Timestamps << "\nsaved=" << settingsSaved
               << "\nexit=" << g_shouldExit << "\nbackgroundLogs=" << count
               << "\ndraft=" << Bytes(editor.Text()) << '\n';
    }
    return 0;
}

static int WithIsolatedSettings(const std::function<int()>& run)
{
    // GUI 验证中的确定按钮只写入本进程临时注册表，不修改用户实际配置
    const auto keyName = L"Software\\SerialTerminalSettingsTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY isolated = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                        nullptr, &isolated, &disposition) != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY)
        return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, isolated) != ERROR_SUCCESS)
    {
        RegCloseKey(isolated);
        return 2;
    }
    g_consoleCloseRequested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_consoleMainThread,
                    SYNCHRONIZE, FALSE, 0);
    auto result = run();
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(isolated);
    RegDeleteTreeW(HKEY_CURRENT_USER, keyName.c_str());
    return result;
}

static void TestRuntimeSettings(UINT action, bool unencodable = false)
{
    auto previousFailures = Failures;
    RegDeleteTreeW(HKEY_CURRENT_USER, L"SOFTWARE\\SerialForWindowsTerminal");
    g_shouldExit = false;
    hInstance = GetModuleHandleW(nullptr);
    TEST_BYTE_PIPE outputPipe;
    boost::asio::io_service io;
    boost::asio::windows::stream_handle output(io, outputPipe.TakeServer());
    CONSOLE_WRITE_QUEUE queue(output);
    SERIAL_CONFIG cfg = {};
    cfg.Serial = 4;
    cfg.BaudRate = 115200;
    cfg.EchoMode = 1;
    cfg.Timestamps = 1;
    COMMAND_HISTORY history;
    LOCAL_LINE_EDITOR editor(history);
    Type(editor, unencodable ? "draft \xf0\x9f\x98\x80" : "draft");
    auto originalDraft = editor.Text();
    TERMINAL_DISPLAY display(io, queue, cfg, editor);
    TERMINAL_SETTINGS_DIALOG settings(io, cfg, display, editor);
    settings.Open(nullptr);
    HWND dialog = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!dialog && std::chrono::steady_clock::now() < deadline)
    {
        EnumWindows([](HWND window, LPARAM context) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && GetDlgItem(window, IDC_BUTTON_EXIT_TERMINAL))
            {
                *reinterpret_cast<HWND*>(context) = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&dialog));
        if (!dialog) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(dialog != nullptr, "runtime settings creates its actual dialog");
    if (dialog)
    {
        settings.Open(nullptr);
        const std::string background = "background log while settings are open\r\n";
        display.Receive(reinterpret_cast<const uint8_t*>(background.data()), background.size(), "[12:00:00.000] ");
        io.run_for(std::chrono::milliseconds(20));
        io.restart();
        DWORD available = 0;
        PeekNamedPipe(outputPipe.Client, nullptr, 0, nullptr, &available, nullptr);
        std::string rendered(available, '\0');
        DWORD read = 0;
        if (available) ReadFile(outputPipe.Client, &rendered[0], available, &read, nullptr);
        Check(rendered.find("background log while settings are open") != std::string::npos &&
              settings.IsOpen() && !display.IsStopped(),
              "terminal output continues while settings are open");
        SendDlgItemMessageW(dialog, IDC_COMBO_ENCODING, CB_SETCURSEL, 1, 0);
        SendDlgItemMessageW(dialog, IDC_COMBO_ECHO, CB_SETCURSEL, 0, 0);
        CheckDlgButton(dialog, IDC_CHECK_TIMESTAMPS, BST_UNCHECKED);
        if (action == WM_CLOSE)
            SendMessageW(dialog, WM_CLOSE, 0, 0);
        else if (action == 0)
            settings.Stop();
        else
            SendMessageW(dialog, WM_COMMAND, action, 0);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (settings.IsOpen() && std::chrono::steady_clock::now() < deadline)
        {
            io.run_for(std::chrono::milliseconds(10));
            io.restart();
            if (settings.IsOpen()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(!settings.IsOpen(), "settings result returns to the terminal event loop");
        const bool applied = action == IDOK && !unencodable;
        Check(cfg.EncodingFormat == (applied ? 1u : 0u) && cfg.EchoMode == (applied ? 0u : 1u) &&
              cfg.Timestamps == (applied ? 0u : 1u), "only a valid OK applies the selected settings");
        HKEY saved = nullptr;
        const bool persisted = RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\SerialForWindowsTerminal", 0,
                                             KEY_READ, &saved) == ERROR_SUCCESS;
        if (saved) RegCloseKey(saved);
        Check(persisted == applied, "cancel, close, exit and encoding rejection do not save settings");
        if (persisted)
        {
            const auto stored = ReadSerialConfig();
            Check(stored.EncodingFormat == 1 && stored.EchoMode == 0 && stored.Timestamps == 0,
                  "OK saves all selected settings to the isolated registry");
        }
        Check(cfg.Serial == 4 && cfg.BaudRate == 115200 && editor.Text() == originalDraft,
              "runtime settings preserves the serial connection and input draft");
        Check(g_shouldExit == (action == IDC_BUTTON_EXIT_TERMINAL), "only Exit requests terminal shutdown");
    }
    settings.Stop();
    display.Cancel();
    queue.Stop();
    output.cancel();
    io.restart();
    io.run();
    if (Failures != previousFailures)
        std::cerr << "Runtime settings action=" << action << " unencodable=" << unencodable
                  << " encoding=" << cfg.EncodingFormat << " local=" << cfg.EchoMode
                  << " timestamps=" << cfg.Timestamps << " exit=" << g_shouldExit << '\n';
}

int wmain(int argc, const wchar_t* argv[])
{
    if (argc < 2) { std::cerr << "Usage: TerminalInteractionTests.exe <isolated-history-path> [--preview | --settings | --runtime-settings]\n"; return 2; }
    if (argc > 2 && std::wstring(argv[2]) == L"--preview")
        return WithIsolatedSettings([&]() { return Preview(argv[1]); });
    if (argc > 2 && std::wstring(argv[2]) == L"--runtime-settings")
        return WithIsolatedSettings([&]() {
            auto parent = argc > 3 ? reinterpret_cast<HWND>(_wcstoui64(argv[3], nullptr, 0)) : nullptr;
            return Preview(argv[1], true, parent);
        });
    if (argc > 2 && std::wstring(argv[2]) == L"--settings")
    {
        hInstance = GetModuleHandleW(nullptr);
        DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTING_DIALOG), GetForegroundWindow(), SettingFunc, 1);
        return 0;
    }
    TestEchoFilter();
    TestReceive();
    TestHistoryAndInput(argv[1]);
    auto submissionHistory = std::wstring(argv[1]) + L".submission";
    TestLocalSubmission(submissionHistory, "echo hi\r", "echo hi\r", "Enter writes one CR without an extra blank submission");
    TestLocalSubmission(submissionHistory, "echo hi\r\n", "echo hi\r", "CRLF console input submits the command only once");
    TestLocalSubmission(submissionHistory, "\r", "\r", "empty Enter writes one CR");
    TestSettingsShortcut(true);
    TestSettingsShortcut(false);
    TestEchoDisplay(true, false, false);
    TestEchoDisplay(false, false, false);
    TestEchoDisplay(true, true, false);
    TestEchoDisplay(true, false, true);
    TestErrorEncoding();
    TestSerialTermination(false, false);
    TestSerialTermination(true, false);
    TestSerialTermination(true, true);
    TestCancelledWrites();
    Check(WithIsolatedSettings([&]() {
        TestRuntimeSettings(IDOK);
        TestRuntimeSettings(IDCANCEL);
        TestRuntimeSettings(WM_CLOSE);
        TestRuntimeSettings(IDC_BUTTON_EXIT_TERMINAL);
        TestRuntimeSettings(IDOK, true);
        TestRuntimeSettings(0);
        return 0;
    }) == 0, "runtime settings checks use an isolated registry");
    std::cout << Checks << " checks, " << Failures << " failures\n";
    return Failures ? 1 : 0;
}
