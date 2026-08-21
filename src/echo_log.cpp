#include "echo_log.h"

#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <vector>

namespace EchoGPPC {
namespace log {

namespace {

// Rolled rather than truncated, so the run that produced a fault is still
// there after the user reproduces it once more.
constexpr long long MAX_BYTES = 4LL * 1024 * 1024;

std::atomic<int> g_level{static_cast<int>(Level::Info)};
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
std::wstring g_path;
std::wstring g_dir;
std::wstring g_process;
bool g_header_written = false;

std::wstring local_appdata()
{
    wchar_t* raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) {
        std::wstring result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    // Falling back to TEMP keeps logging alive on a locked-down profile.
    wchar_t buf[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
}

std::wstring process_name()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"?";
    }
    buf[n] = L'\0';
    const wchar_t* slash = wcsrchr(buf, L'\\');
    return slash ? std::wstring(slash + 1) : std::wstring(buf);
}

void roll_if_large(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return;
    }
    const long long size =
        (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size < MAX_BYTES) {
        return;
    }
    const std::wstring old = path + L".1";
    DeleteFileW(old.c_str());
    MoveFileW(path.c_str(), old.c_str());
}

}  // namespace

std::wstring log_directory()
{
    if (!g_dir.empty()) {
        return g_dir;
    }
    std::wstring dir = local_appdata();
    if (!dir.empty() && dir.back() != L'\\') {
        dir += L'\\';
    }
    dir += L"EchoGPPC";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void init(const wchar_t* tag) noexcept
{
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    EnterCriticalSection(&g_lock);
    if (g_path.empty()) {
        g_dir = log_directory();
        g_process = process_name();
        wchar_t name[64];
        // The pid is in the filename because two hosts (say NVDA and a
        // browser) commonly run the same engine at the same time, and
        // interleaved lines from both are unreadable.
        _snwprintf_s(name, _countof(name), _TRUNCATE, L"\\echogppc-%s-%lu.log",
                     tag ? tag : L"unknown", GetCurrentProcessId());
        g_path = g_dir + name;
        roll_if_large(g_path);
    }
    LeaveCriticalSection(&g_lock);
}

void set_level(Level level) noexcept
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level level() noexcept
{
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

Level level_from_string(const std::wstring& s, Level fallback) noexcept
{
    struct { const wchar_t* name; Level value; } table[] = {
        {L"off", Level::Off},     {L"none", Level::Off},
        {L"error", Level::Error}, {L"warn", Level::Warn},
        {L"warning", Level::Warn},{L"info", Level::Info},
        {L"debug", Level::Debug}, {L"trace", Level::Trace},
    };
    for (const auto& entry : table) {
        if (_wcsicmp(s.c_str(), entry.name) == 0) {
            return entry.value;
        }
    }
    return fallback;
}

const wchar_t* level_to_string(Level level) noexcept
{
    switch (level) {
        case Level::Off:   return L"off";
        case Level::Error: return L"error";
        case Level::Warn:  return L"warn";
        case Level::Info:  return L"info";
        case Level::Debug: return L"debug";
        case Level::Trace: return L"trace";
    }
    return L"info";
}

std::wstring current_path()
{
    return g_path;
}

namespace {

// Appends `text` to the log as UTF-8 bytes.
//
// Written as raw bytes on a binary handle rather than through a stream opened
// with ccs=UTF-8: in that mode the CRT puts the stream in wide orientation and
// narrow writes silently do nothing, which is exactly the failure this
// replaced -- a log file containing a byte-order mark and not one line.
void append_utf8(const std::wstring& text)
{
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return;
    }
    std::vector<char> bytes(static_cast<size_t>(needed));
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        bytes.data(), needed, nullptr, nullptr);

    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
}

}  // namespace

void write(Level lvl, const wchar_t* fmt, ...) noexcept
{
    if (!g_lock_ready || g_path.empty() || !fmt) {
        return;
    }

    wchar_t body[2048];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) {
        // Truncation still leaves what fitted; a hard failure leaves the
        // buffer undefined, so say so rather than emit garbage.
        wcscpy_s(body, L"<log message could not be formatted>");
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    static const wchar_t* const kNames[] = {L"OFF", L"ERROR", L"WARN",
                                            L"INFO", L"DEBUG", L"TRACE"};
    const int index = static_cast<int>(lvl);
    const wchar_t* name = (index >= 0 && index <= 5) ? kNames[index] : L"?";

    wchar_t line[2304];

    EnterCriticalSection(&g_lock);
    if (!g_header_written) {
        g_header_written = true;
        wchar_t header[512];
        _snwprintf_s(header, _countof(header), _TRUNCATE,
                     L"\r\n=== Echo GPPC log opened %04d-%02d-%02d %02d:%02d:%02d "
                     L"host=%ls pid=%lu %d-bit ===\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     g_process.c_str(), GetCurrentProcessId(),
                     static_cast<int>(sizeof(void*) * 8));
        append_utf8(header);
    }
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"%02d:%02d:%02d.%03d [%-5ls] [t%lu] %ls\r\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 name, GetCurrentThreadId(), body);
    append_utf8(line);
    LeaveCriticalSection(&g_lock);
}

}  // namespace log
}  // namespace EchoGPPC
