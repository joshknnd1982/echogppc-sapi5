#pragma once

// Diagnostic log shared by the SAPI5 engines and the configuration utility.
//
// A SAPI engine runs inside somebody else's process with no console and no
// window, so when it misbehaves the log file is the only evidence there is.
// It is therefore on by default at Info, and every entry carries the process
// name and thread id -- several hosts load the engine into several processes
// at once and an unattributed line is worse than no line.

#include <windows.h>
#include <string>

namespace EchoGPPC {
namespace log {

enum class Level : int {
    Off   = 0,
    Error = 1,
    Warn  = 2,
    Info  = 3,
    Debug = 4,
    Trace = 5,
};

// `tag` names the log file (e.g. L"sapi64", L"config"). Safe to call more
// than once; the first call wins. Never throws and never fails in a way the
// caller must handle: logging that cannot start must not stop speech.
void init(const wchar_t* tag) noexcept;

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;

// Parses "off", "error", "warn", "info", "debug", "trace" (case-insensitive).
// Anything unrecognised gives back `fallback`.
[[nodiscard]] Level level_from_string(const std::wstring& s, Level fallback) noexcept;
[[nodiscard]] const wchar_t* level_to_string(Level level) noexcept;

// The file this process is writing to, for the utility's "open log" button.
[[nodiscard]] std::wstring current_path();

// The directory logs live in, created on demand.
[[nodiscard]] std::wstring log_directory();

void write(Level level, const wchar_t* fmt, ...) noexcept;

}  // namespace log
}  // namespace EchoGPPC

// Checking the level before formatting keeps the cost of a disabled Trace
// call down to one relaxed load, which matters inside the audio read loop.
//
// `L"" fmt` widens the caller's literal by adjacent-literal concatenation, so
// call sites still read as ordinary narrow format strings while the formatting
// itself happens wide. That matters because almost everything worth logging
// here is a wchar_t path or voice name, and formatting those through a narrow
// printf goes via the C locale -- which a DLL living in someone else's process
// must not touch, and which mangles or drops anything non-ASCII. So in these
// format strings %ls (or %s) is a wide string and %hs is a narrow one.
#define ECHO_LOG(lvl, fmt, ...)                                               \
    do {                                                                      \
        if (::EchoGPPC::log::level() >= ::EchoGPPC::log::Level::lvl) {        \
            ::EchoGPPC::log::write(::EchoGPPC::log::Level::lvl, L"" fmt,      \
                                   ##__VA_ARGS__);                            \
        }                                                                     \
    } while (0)

#define ECHO_ERR(fmt, ...)   ECHO_LOG(Error, fmt, ##__VA_ARGS__)
#define ECHO_WARN(fmt, ...)  ECHO_LOG(Warn,  fmt, ##__VA_ARGS__)
#define ECHO_INFO(fmt, ...)  ECHO_LOG(Info,  fmt, ##__VA_ARGS__)
#define ECHO_DEBUG(fmt, ...) ECHO_LOG(Debug, fmt, ##__VA_ARGS__)
#define ECHO_TRACE(fmt, ...) ECHO_LOG(Trace, fmt, ##__VA_ARGS__)
