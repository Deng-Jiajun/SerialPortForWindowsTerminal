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
    Check(editor.Feed(3, CP_UTF8) == INPUT_ACTION::Interrupt && editor.Text().empty(), "Ctrl+C clears input and interrupts device");
    Check(editor.Feed(4, CP_UTF8) == INPUT_ACTION::EndOfFile, "empty Ctrl+D reaches device");
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
    DoConsoleToSerial(input, serialQueue, inputBuffer, cfg, display, editor, history, automation);
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

static int Preview(const std::wstring& path)
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
    display.Start();
    Type(editor, "ma");
    display.Redraw();
    auto send = [&](const std::string& data) { display.Receive(reinterpret_cast<const uint8_t*>(data.data()), data.size(), LocalTimestamp()); };
    send("[DEBUG] connected to test device\r\n");
    send("[WARN] reconnecting\r\n");
    send("[ERROR] connection timed out\r\n");
    send("[DEBUG] 中文日志，输入草稿与历史建议保持显示\r\n");
    boost::asio::steady_timer ticker(io);
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
            if (count < 120) tick(); else display.Stop();
        });
    };
    tick();
    io.run();
    return 0;
}

int wmain(int argc, const wchar_t* argv[])
{
    if (argc < 2) { std::cerr << "Usage: TerminalInteractionTests.exe <isolated-history-path> [--preview | --settings]\n"; return 2; }
    if (argc > 2 && std::wstring(argv[2]) == L"--preview") return Preview(argv[1]);
    if (argc > 2 && std::wstring(argv[2]) == L"--settings")
    {
        hInstance = GetModuleHandleW(nullptr);
        DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTING_DIALOG), GetForegroundWindow(), SettingFunc, 1);
        return 0;
    }
    TestReceive();
    TestHistoryAndInput(argv[1]);
    auto submissionHistory = std::wstring(argv[1]) + L".submission";
    TestLocalSubmission(submissionHistory, "echo hi\r", "echo hi\r", "Enter writes one CR without an extra blank submission");
    TestLocalSubmission(submissionHistory, "echo hi\r\n", "echo hi\r", "CRLF console input submits the command only once");
    TestLocalSubmission(submissionHistory, "\r", "\r", "empty Enter writes one CR");
    std::cout << Checks << " checks, " << Failures << " failures\n";
    return Failures ? 1 : 0;
}
