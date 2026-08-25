#pragma once

#include <cstdint>

namespace serial_automation
{
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SerialForWindowsTerminal";
    constexpr std::uint32_t kProtocolMagic = 0x54465753; // "SFWT"
    constexpr std::uint32_t kProtocolVersion = 1;
    constexpr std::uint32_t kMaxTextSize = 64 * 1024;
    constexpr std::uint32_t kMaxResponseSize = 4 * 1024 * 1024;

    enum LINE_ENDING : std::uint32_t
    {
        LINE_ENDING_NONE = 0,
        LINE_ENDING_CR,
        LINE_ENDING_LF,
        LINE_ENDING_CRLF
    };

    enum RESPONSE_STATUS : std::uint32_t
    {
        RESPONSE_STATUS_SUCCESS = 0,
        RESPONSE_STATUS_TIMEOUT,
        RESPONSE_STATUS_INVALID_REQUEST,
        RESPONSE_STATUS_SERIAL_ERROR,
        RESPONSE_STATUS_RESPONSE_TOO_LARGE,
        RESPONSE_STATUS_SERVER_STOPPED
    };

    enum COMPLETION_REASON : std::uint32_t
    {
        COMPLETION_REASON_NONE = 0,
        COMPLETION_REASON_PROMPT,
        COMPLETION_REASON_IDLE,
        COMPLETION_REASON_TIMEOUT,
        COMPLETION_REASON_ERROR
    };

#pragma pack(push, 1)
    struct REQUEST_HEADER
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        std::uint32_t CommandSize;
        std::uint32_t PromptSize;
        std::uint32_t IdleTimeoutMs;
        std::uint32_t TotalTimeoutMs;
        std::uint32_t LineEnding;
    };

    struct RESPONSE_HEADER
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        std::uint32_t Status;
        std::uint32_t CompletionReason;
        std::uint32_t EncodingFormat;
        std::uint32_t DataSize;
    };
#pragma pack(pop)
}
