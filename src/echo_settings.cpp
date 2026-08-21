#include "echo_settings.h"

#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace EchoGPPC {

namespace {

constexpr int      PITCH_MAX  = 63;
constexpr int      VOLUME_MAX = 15;
constexpr int      DELAY_MAX  = 15;
constexpr int      REPEAT_MAX = 99;
constexpr double   MULT_MIN   = 0.25;
constexpr double   MULT_MAX   = 4.0;

std::wstring roaming_appdata()
{
    wchar_t* raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        std::wstring result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    wchar_t buf[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
}

std::wstring trim(const std::wstring& s)
{
    const auto first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Read the whole file as UTF-8 (with or without BOM) and widen it. Written by
// hand rather than through GetPrivateProfileString because that API keeps its
// own cache of the last INI it touched, which defeats the whole point of
// watching the file for outside changes.
bool read_text_file(const std::wstring& path, std::wstring* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = bytes.empty() ||
                    (ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                     read == bytes.size());
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    if (bytes.empty()) {
        out->clear();
        return true;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    out->assign(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        out->data(), needed);
    return true;
}

bool write_text_file(const std::wstring& path, const std::wstring& text)
{
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        bytes.data(), needed, nullptr, nullptr);

    // Written to a sibling temp file and renamed over the top, so a reader
    // that stats the file mid-save never sees a half-written INI.
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

int to_int(const std::wstring& s, int fallback)
{
    wchar_t* end = nullptr;
    const long value = wcstol(s.c_str(), &end, 10);
    return (end && end != s.c_str()) ? static_cast<int>(value) : fallback;
}

double to_double(const std::wstring& s, double fallback)
{
    wchar_t* end = nullptr;
    const double value = wcstod(s.c_str(), &end);
    return (end && end != s.c_str()) ? value : fallback;
}

bool to_bool(const std::wstring& s, bool fallback)
{
    if (s.empty()) {
        return fallback;
    }
    if (_wcsicmp(s.c_str(), L"1") == 0 || _wcsicmp(s.c_str(), L"true") == 0 ||
        _wcsicmp(s.c_str(), L"yes") == 0 || _wcsicmp(s.c_str(), L"on") == 0) {
        return true;
    }
    if (_wcsicmp(s.c_str(), L"0") == 0 || _wcsicmp(s.c_str(), L"false") == 0 ||
        _wcsicmp(s.c_str(), L"no") == 0 || _wcsicmp(s.c_str(), L"off") == 0) {
        return false;
    }
    return fallback;
}

std::wstring from_double(double value)
{
    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.4f", value);
    // Trailing zeros make the file look machine-generated and are noise for
    // anyone editing it by hand.
    std::wstring s(buf);
    while (s.size() > 1 && s.back() == L'0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == L'.') {
        s.pop_back();
    }
    return s;
}

std::wstring from_int(long long value)
{
    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%lld", value);
    return buf;
}

using Section = std::map<std::wstring, std::wstring>;

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

using Ini = std::map<std::wstring, Section, CaseInsensitiveLess>;

Ini parse_ini(const std::wstring& text)
{
    Ini ini;
    std::wstring current = L"General";
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        const std::wstring line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }
        if (line.front() == L'[' && line.back() == L']') {
            current = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        ini[current][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        if (pos > text.size()) {
            break;
        }
    }
    return ini;
}

const std::wstring* find(const Section& section, const wchar_t* key)
{
    for (const auto& [k, v] : section) {
        if (_wcsicmp(k.c_str(), key) == 0) {
            return &v;
        }
    }
    return nullptr;
}

std::wstring get(const Section& section, const wchar_t* key, const std::wstring& fallback = {})
{
    const std::wstring* found = find(section, key);
    return found ? *found : fallback;
}

constexpr wchar_t VOICE_PREFIX[] = L"Voice.";

}  // namespace

void VoiceSettings::clamp() noexcept
{
    pitch         = std::clamp(pitch, 0, PITCH_MAX);
    volume        = std::clamp(volume, 0, VOLUME_MAX);
    word_delay    = std::clamp(word_delay, 0, DELAY_MAX);
    repeat_filter = std::clamp(repeat_filter, 0, REPEAT_MAX);
    speed         = std::clamp(speed, MULT_MIN, MULT_MAX);
    clock         = std::clamp(clock, MULT_MIN, MULT_MAX);
    frame_rate    = std::clamp(frame_rate, 0, 3);
    letter_mode   = std::clamp(letter_mode, 0, 1);
    punctuation   = std::clamp(punctuation, 0, 2);
    index_break   = std::clamp(index_break, 0, 1);
    chunk_size    = std::clamp<unsigned>(chunk_size, 8, 80);

    // Only the rates the library is documented to accept; anything else is
    // snapped to the nearest one rather than passed through and refused.
    static constexpr unsigned kRates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};
    unsigned best = kRates[0];
    unsigned best_gap = 0xFFFFFFFFu;
    for (unsigned rate : kRates) {
        const unsigned gap = rate > sample_rate ? rate - sample_rate : sample_rate - rate;
        if (gap < best_gap) {
            best_gap = gap;
            best = rate;
        }
    }
    sample_rate = best;
}

VoiceSettings Settings::for_voice(const std::wstring& stem) const
{
    const auto it = voices.find(stem);
    VoiceSettings result = (it != voices.end()) ? it->second : VoiceSettings{};
    result.clamp();
    return result;
}

void Settings::set_for_voice(const std::wstring& stem, const VoiceSettings& value)
{
    VoiceSettings copy = value;
    copy.clamp();
    voices[stem] = copy;
}

std::wstring settings_path()
{
    std::wstring dir = roaming_appdata();
    if (!dir.empty() && dir.back() != L'\\') {
        dir += L'\\';
    }
    dir += L"EchoGPPC";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

bool load_settings(const std::wstring& path, Settings* out)
{
    if (!out) {
        return false;
    }
    *out = Settings{};

    std::wstring text;
    if (!read_text_file(path, &text)) {
        return false;
    }
    const Ini ini = parse_ini(text);

    const auto general = ini.find(L"General");
    if (general != ini.end()) {
        out->default_voice = get(general->second, L"Voice");
        out->log_level = log::level_from_string(get(general->second, L"LogLevel", L"info"),
                                                log::Level::Info);
    }

    for (const auto& [name, section] : ini) {
        if (_wcsnicmp(name.c_str(), VOICE_PREFIX, wcslen(VOICE_PREFIX)) != 0) {
            continue;
        }
        const std::wstring stem = name.substr(wcslen(VOICE_PREFIX));
        if (stem.empty()) {
            continue;
        }
        VoiceSettings v;
        v.pitch         = to_int(get(section, L"Pitch"), v.pitch);
        v.volume        = to_int(get(section, L"Volume"), v.volume);
        v.word_delay    = to_int(get(section, L"WordDelay"), v.word_delay);
        v.repeat_filter = to_int(get(section, L"RepeatFilter"), v.repeat_filter);
        v.speed         = to_double(get(section, L"Speed"), v.speed);
        v.clock         = to_double(get(section, L"ChipClock"), v.clock);
        v.sample_rate   = static_cast<unsigned>(
            to_int(get(section, L"SampleRate"), static_cast<int>(v.sample_rate)));
        v.monotone      = to_bool(get(section, L"Monotone"), v.monotone);
        v.compressed    = to_bool(get(section, L"Compressed"), v.compressed);
        v.frame_rate    = to_int(get(section, L"FrameRate"), v.frame_rate);
        v.raw           = to_bool(get(section, L"Raw"), v.raw);
        v.letter_mode   = to_int(get(section, L"LetterMode"), v.letter_mode);
        v.punctuation   = to_int(get(section, L"Punctuation"), v.punctuation);
        v.chunk_size    = static_cast<unsigned>(
            to_int(get(section, L"ChunkSize"), static_cast<int>(v.chunk_size)));
        v.index_break   = to_int(get(section, L"IndexBreak"), v.index_break);
        v.clamp();
        out->voices[stem] = v;
    }
    return true;
}

bool save_settings(const std::wstring& path, const Settings& in)
{
    std::wstring text;
    text += L"; Echo GPPC SAPI5 settings.\r\n";
    text += L"; Written by the Echo GPPC Configuration utility; safe to edit by hand.\r\n";
    text += L"; Changes take effect on the next utterance -- no restart needed.\r\n";
    text += L"\r\n[General]\r\n";
    text += L"Voice=" + in.default_voice + L"\r\n";
    text += L"LogLevel=";
    text += log::level_to_string(in.log_level);
    text += L"\r\n";

    for (const auto& [stem, v] : in.voices) {
        text += L"\r\n[" + std::wstring(VOICE_PREFIX) + stem + L"]\r\n";
        text += L"Pitch="         + from_int(v.pitch)         + L"          ; 0-63\r\n";
        text += L"Volume="        + from_int(v.volume)        + L"          ; 0-15\r\n";
        text += L"WordDelay="     + from_int(v.word_delay)    + L"          ; 0-15, Textalker 3 only\r\n";
        text += L"RepeatFilter="  + from_int(v.repeat_filter) + L"          ; 0-99, Textalker 3 only\r\n";
        text += L"Speed="         + from_double(v.speed)      + L"          ; 0.25-4.0, pitch preserved\r\n";
        text += L"ChipClock="     + from_double(v.clock)      + L"          ; 0.25-4.0, speed and pitch\r\n";
        text += L"SampleRate="    + from_int(v.sample_rate)   + L"          ; 8000/11025/16000/22050/32000/44100/48000\r\n";
        text += L"Monotone="      + from_int(v.monotone ? 1 : 0)   + L"          ; 0 or 1\r\n";
        text += L"Compressed="    + from_int(v.compressed ? 1 : 0) + L"          ; 0 or 1\r\n";
        text += L"FrameRate="     + from_int(v.frame_rate)    + L"          ; 0-3, the chip's own four-step rate\r\n";
        text += L"; --- advanced ---\r\n";
        text += L"Raw="           + from_int(v.raw ? 1 : 0)   + L"          ; 1 disables UTF-8/typographic folding\r\n";
        text += L"LetterMode="    + from_int(v.letter_mode)   + L"          ; 0-1 (no audible effect in ABI 6)\r\n";
        text += L"Punctuation="   + from_int(v.punctuation)   + L"          ; 0-2 (no audible effect in ABI 6)\r\n";
        text += L"ChunkSize="     + from_int(v.chunk_size)    + L"          ; 8-80, Textalker's text buffer\r\n";
        text += L"IndexBreak="    + from_int(v.index_break)   + L"          ; 0-1\r\n";
    }
    return write_text_file(path, text);
}

SettingsWatcher::SettingsWatcher()
{
    InitializeCriticalSection(&lock_);
    path_ = settings_path();
}

SettingsWatcher::~SettingsWatcher()
{
    DeleteCriticalSection(&lock_);
}

bool SettingsWatcher::refresh()
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool exists = GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &info) != 0;

    EnterCriticalSection(&lock_);
    bool changed = false;
    if (!exists) {
        // No file yet: built-in defaults, and only say so once.
        if (!loaded_) {
            loaded_ = true;
            changed = true;
            settings_ = Settings{};
            stamp_ = FILETIME{};
            size_ = 0;
        }
    } else {
        const ULONGLONG size =
            (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (!loaded_ || size != size_ ||
            CompareFileTime(&info.ftLastWriteTime, &stamp_) != 0) {
            Settings fresh;
            if (load_settings(path_, &fresh)) {
                settings_ = fresh;
                stamp_ = info.ftLastWriteTime;
                size_ = size;
                loaded_ = true;
                changed = true;
            }
        }
    }
    const log::Level level = settings_.log_level;
    LeaveCriticalSection(&lock_);

    if (changed) {
        log::set_level(level);
    }
    return changed;
}

Settings SettingsWatcher::snapshot()
{
    EnterCriticalSection(&lock_);
    Settings copy = settings_;
    LeaveCriticalSection(&lock_);
    return copy;
}

}  // namespace EchoGPPC
