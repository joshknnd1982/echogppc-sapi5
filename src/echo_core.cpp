#include "echo_core.h"
#include "echo_log.h"
#include "utils.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace EchoGPPC {

namespace {

#if defined(_WIN64)
constexpr wchar_t ECHOTALK_DLL[] = L"echotalk64.dll";
#else
constexpr wchar_t ECHOTALK_DLL[] = L"echotalk32.dll";
#endif

// A local so GetModuleHandleEx can find the module this code lives in --
// taking the address of a function is the only portable way to ask "which
// DLL am I?" from inside a DLL that does not stash its own HINSTANCE.
void module_anchor() {}

std::wstring this_module_directory()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_anchor), &self)) {
        return {};
    }
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    path[n] = L'\0';
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return {};
    }
    *slash = L'\0';
    return std::wstring(path);
}

std::wstring parent_of(const std::wstring& dir)
{
    const auto slash = dir.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash);
}

bool file_exists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ANSI is what the library's create() takes for its paths. A path that cannot
// be represented in the active code page would silently become question marks
// and fail to open, so that case is detected and reported rather than guessed.
bool to_ansi_path(const std::wstring& in, std::string* out)
{
    BOOL lossy = FALSE;
    const int needed = WideCharToMultiByte(CP_ACP, 0, in.c_str(), -1, nullptr, 0, nullptr, &lossy);
    if (needed <= 0) {
        return false;
    }
    out->assign(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, in.c_str(), -1, out->data(), needed, nullptr, &lossy);
    return !lossy;
}

}  // namespace

Core::Core()
{
    InitializeCriticalSection(&lock_);
}

Core::~Core()
{
    destroy_locked();
    if (lib_) {
        FreeLibrary(lib_);
        lib_ = nullptr;
    }
    DeleteCriticalSection(&lock_);
}

Core& Core::instance()
{
    // Function-local static: the C++11 guarantee makes construction -- and
    // therefore InitializeCriticalSection -- thread-safe without a second
    // lock of our own, which is why lock_ is ready before any caller can
    // reach EnterCriticalSection below.
    static Core core;
    return core;
}

unsigned Core::effective_sample_rate(unsigned requested, double clock) noexcept
{
    const unsigned native = static_cast<unsigned>(8000.0 * clock + 0.5);
    return (std::max)(requested, native);
}

std::wstring Core::media_directory()
{
    if (!media_dir_.empty()) {
        return media_dir_;
    }
    // The 32-bit engine is installed in an x86 subdirectory while the ROM
    // pairs live once in the install root, so look beside this module first
    // and then one level up rather than shipping two copies of the ROMs.
    const std::wstring here = this_module_directory();
    for (const std::wstring& dir : {here, parent_of(here)}) {
        if (dir.empty()) {
            continue;
        }
        if (file_exists(dir + L"\\" + ECHOTALK_DLL)) {
            media_dir_ = dir;
            return media_dir_;
        }
    }
    media_dir_ = here;
    return media_dir_;
}

bool Core::ensure_loaded(std::wstring* error)
{
    EnterCriticalSection(&lock_);

    if (lib_) {
        LeaveCriticalSection(&lock_);
        return true;
    }

    const std::wstring dir = media_directory();
    const std::wstring dll = dir + L"\\" + ECHOTALK_DLL;
    ECHO_INFO("Core: loading emulator library %ls", dll.c_str());

    lib_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib_) {
        const DWORD err = GetLastError();
        if (error) {
            *error = L"Could not load " + dll;
        }
        ECHO_ERR("Core: LoadLibrary failed for %ls (GetLastError=%lu)", dll.c_str(), err);
        LeaveCriticalSection(&lock_);
        return false;
    }

    // A missing export means the DLL beside us is not the one this engine was
    // built for. Failing here is far better than calling through a null.
    bool ok = true;
    auto resolve = [&](auto& target, const char* name) {
        FARPROC proc = GetProcAddress(lib_, name);
        if (!proc) {
            ECHO_ERR("Core: export %hs missing from %ls", name, ECHOTALK_DLL);
            ok = false;
            return;
        }
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(proc);
    };

    resolve(p_abi_version,          "echotalk_abi_version");
    resolve(p_create,               "echotalk_create");
    resolve(p_destroy,              "echotalk_destroy");
    resolve(p_version,              "echotalk_version");
    resolve(p_set_pitch,            "echotalk_set_pitch");
    resolve(p_set_flat,             "echotalk_set_flat");
    resolve(p_set_volume,           "echotalk_set_volume");
    resolve(p_set_word_delay,       "echotalk_set_word_delay");
    resolve(p_set_repeat_filter,    "echotalk_set_repeat_filter");
    resolve(p_set_compressed,       "echotalk_set_compressed");
    resolve(p_set_frame_rate,       "echotalk_set_frame_rate");
    resolve(p_set_raw,              "echotalk_set_raw");
    resolve(p_set_letter_mode,      "echotalk_set_letter_mode");
    resolve(p_set_punctuation,      "echotalk_set_punctuation");
    resolve(p_set_index_break,      "echotalk_set_index_break");
    resolve(p_set_speed,            "echotalk_set_speed");
    resolve(p_set_clock_multiplier, "echotalk_set_clock_multiplier");
    resolve(p_set_sample_rate,      "echotalk_set_sample_rate");
    resolve(p_set_chunk_size,       "echotalk_set_chunk_size");
    resolve(p_sample_rate,          "echotalk_sample_rate");
    resolve(p_speak,                "echotalk_speak");
    resolve(p_read,                 "echotalk_read");
    resolve(p_available,            "echotalk_available");
    resolve(p_pending,              "echotalk_pending");
    resolve(p_next_index,           "echotalk_next_index");
    resolve(p_stop,                 "echotalk_stop");
    resolve(p_overruns,             "echotalk_overruns");
    resolve(p_command_errors,       "echotalk_command_errors");
    resolve(p_clear_command_errors, "echotalk_clear_command_errors");

    if (!ok) {
        FreeLibrary(lib_);
        lib_ = nullptr;
        if (error) {
            *error = ECHOTALK_DLL + std::wstring(L" is missing expected exports");
        }
        LeaveCriticalSection(&lock_);
        return false;
    }

    abi_ = p_abi_version();
    if (abi_ != REQUIRED_ABI) {
        ECHO_ERR("Core: ABI mismatch -- library reports %u, engine needs %u", abi_, REQUIRED_ABI);
        FreeLibrary(lib_);
        lib_ = nullptr;
        if (error) {
            *error = L"echotalk ABI mismatch";
        }
        LeaveCriticalSection(&lock_);
        return false;
    }

    ECHO_INFO("Core: emulator library loaded, ABI %u, media directory %ls", abi_, dir.c_str());
    LeaveCriticalSection(&lock_);
    return true;
}

std::vector<VoiceRom> Core::voices()
{
    std::wstring error;
    if (!ensure_loaded(&error)) {
        return {};
    }

    EnterCriticalSection(&lock_);
    if (voices_scanned_) {
        std::vector<VoiceRom> copy = voices_;
        LeaveCriticalSection(&lock_);
        return copy;
    }

    const std::wstring dir = media_directory();
    std::vector<VoiceRom> found;

    // Both the emulator's own directory and its parent are searched, and the
    // ROMs need not sit with the emulator. That matters for the installed
    // layout: the 32-bit engine and echotalk32.dll live in an x86
    // subdirectory while the Textalker ROM pairs live once in the install
    // root, so neither architecture needs its own copy of them.
    std::vector<std::wstring> search_dirs{dir};
    const std::wstring parent = parent_of(dir);
    if (!parent.empty() && parent != dir) {
        search_dirs.push_back(parent);
    }

    // A pair is <stem>.ram.bin (or <stem>.loader.bin) plus <stem>.obj.bin --
    // the naming the upstream project's own roms/ directory uses. Scanning
    // rather than hard-coding means a ROM pair this build has never heard of
    // still shows up, labelled with whatever banner it reports.
    for (const std::wstring& scan : search_dirs) {
        for (const wchar_t* suffix : {L".ram.bin", L".loader.bin"}) {
            WIN32_FIND_DATAW fd{};
            const std::wstring pattern = scan + L"\\*" + suffix;
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) {
                continue;
            }
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                const std::wstring name(fd.cFileName);
                const size_t suffix_len = wcslen(suffix);
                if (name.size() <= suffix_len) {
                    continue;
                }
                const std::wstring stem = name.substr(0, name.size() - suffix_len);
                const std::wstring obj = scan + L"\\" + stem + L".obj.bin";
                if (!file_exists(obj)) {
                    ECHO_WARN("Core: %ls has no matching .obj.bin, skipped", name.c_str());
                    continue;
                }
                // First directory wins, so a ROM beside the engine overrides
                // one in the install root rather than appearing twice.
                if (std::any_of(found.begin(), found.end(),
                                [&](const VoiceRom& v) { return v.stem == stem; })) {
                    continue;
                }
                VoiceRom rom;
                rom.stem = stem;
                rom.loader = scan + L"\\" + name;
                rom.obj = obj;
                found.push_back(rom);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    // Stable order so the voice list does not shuffle between runs.
    std::sort(found.begin(), found.end(),
              [](const VoiceRom& a, const VoiceRom& b) { return a.stem < b.stem; });

    // Boot each pair just long enough to read its banner, so a voice is
    // labelled with what the ROM says it is rather than what it is called.
    // Only one machine may exist at a time, hence destroy before the next.
    std::vector<VoiceRom> usable;
    for (VoiceRom& rom : found) {
        destroy_locked();
        std::wstring boot_error;
        if (!boot_locked(rom, &boot_error)) {
            ECHO_WARN("Core: ROM pair '%ls' is unusable: %ls", rom.stem.c_str(),
                      boot_error.c_str());
            continue;
        }
        const char* banner = p_version(handle_);
        rom.banner = banner ? utils::string_to_wstring(banner) : L"";
        if (rom.banner.empty()) {
            rom.banner = L"unknown";
        }
        rom.display = L"Echo GPPC Textalker " + rom.banner;
        ECHO_INFO("Core: voice '%ls' -> %ls", rom.stem.c_str(), rom.display.c_str());
        usable.push_back(rom);
    }
    destroy_locked();

    voices_ = usable;
    voices_scanned_ = true;
    if (usable.empty()) {
        ECHO_ERR("Core: no usable Textalker ROM pairs in %ls", dir.c_str());
    }
    std::vector<VoiceRom> copy = voices_;
    LeaveCriticalSection(&lock_);
    return copy;
}

bool Core::boot_locked(const VoiceRom& rom, std::wstring* error)
{
    std::string loader_ansi;
    std::string obj_ansi;
    if (!to_ansi_path(rom.loader, &loader_ansi) || !to_ansi_path(rom.obj, &obj_ansi)) {
        if (error) {
            *error = L"ROM path cannot be represented in the system code page";
        }
        return false;
    }

    char err_buf[256] = {0};
    handle_ = p_create(loader_ansi.c_str(), obj_ansi.c_str(), err_buf, sizeof(err_buf));
    if (!handle_) {
        if (error) {
            *error = utils::string_to_wstring(err_buf);
        }
        loaded_stem_.clear();
        return false;
    }
    loaded_stem_ = rom.stem;
    return true;
}

void Core::destroy_locked()
{
    if (handle_) {
        p_destroy(handle_);
        handle_ = nullptr;
    }
    loaded_stem_.clear();
}

void Core::apply_locked(const CoreParams& params)
{
    void* h = handle_;
    if (!h) {
        return;
    }
    // Pitch and monotone are one Textalker setting, not two: sending nP where
    // the voice is meant to be flat would quietly un-flatten it.
    p_set_pitch(h, params.pitch);
    p_set_flat(h, params.monotone ? 1 : 0);
    p_set_volume(h, params.volume);
    p_set_word_delay(h, params.word_delay);
    p_set_repeat_filter(h, params.repeat_filter);
    p_set_compressed(h, params.compressed ? 1 : 0);
    p_set_frame_rate(h, params.frame_rate);
    p_set_raw(h, params.raw ? 1 : 0);
    p_set_letter_mode(h, params.letter_mode);
    p_set_punctuation(h, params.punctuation);
    p_set_index_break(h, params.index_break);
    p_set_chunk_size(h, params.chunk_size);
    p_set_speed(h, params.speed);
    p_set_clock_multiplier(h, params.clock);
    p_set_sample_rate(h, effective_sample_rate(params.sample_rate, params.clock));
    p_clear_command_errors(h);
}

// --- Session ------------------------------------------------------------

Core::Session::Session(Core& core, const VoiceRom& rom, const CoreParams& params)
    : core_(core)
{
    std::wstring error;
    if (!core_.ensure_loaded(&error)) {
        error_ = error;
        return;
    }

    EnterCriticalSection(&core_.lock_);
    locked_ = true;

    // A voice change really is a new machine: the 6502 and all of its memory
    // are reset, so Textalker comes back at ITS defaults and every setting
    // has to be pushed again -- which apply_locked does unconditionally.
    if (!core_.handle_ || core_.loaded_stem_ != rom.stem) {
        core_.destroy_locked();
        if (!core_.boot_locked(rom, &error_)) {
            ECHO_ERR("Session: could not boot ROM '%ls': %ls", rom.stem.c_str(), error_.c_str());
            return;  // the destructor releases the lock, because locked_ is set
        }
        ECHO_DEBUG("Session: booted ROM '%ls'", rom.stem.c_str());
    } else {
        // Same machine as last time, but it may still hold audio and index
        // marks from an utterance that was cut short.
        core_.p_stop(core_.handle_);
    }

    core_.apply_locked(params);
    handle_ = core_.handle_;
}

Core::Session::~Session()
{
    if (handle_) {
        // Leave nothing queued for whoever takes the machine next: an
        // abandoned utterance's audio and index marks would otherwise be
        // inherited by the next one.
        core_.p_stop(handle_);
    }
    // One flag decides this, rather than inferring it from handle_: the boot
    // can fail after the lock was taken, and ensure_loaded can fail before it
    // ever was. Getting that wrong deadlocks every voice in the process.
    if (locked_) {
        LeaveCriticalSection(&core_.lock_);
        locked_ = false;
    }
}

unsigned Core::Session::sample_rate() const noexcept
{
    return handle_ ? core_.p_sample_rate(handle_) : 0;
}

bool Core::Session::speak(const std::string& utf8_text)
{
    if (!handle_) {
        return false;
    }
    return core_.p_speak(handle_, utf8_text.c_str()) == 0;
}

size_t Core::Session::read(short* buffer, size_t max_samples)
{
    return handle_ ? core_.p_read(handle_, buffer, max_samples) : 0;
}

bool Core::Session::next_index(int* out)
{
    return handle_ && core_.p_next_index(handle_, out) != 0;
}

void Core::Session::stop()
{
    if (handle_) {
        core_.p_stop(handle_);
    }
}

unsigned Core::Session::overruns() const noexcept
{
    return handle_ ? core_.p_overruns(handle_) : 0;
}

unsigned Core::Session::command_errors() const noexcept
{
    return handle_ ? core_.p_command_errors(handle_) : 0;
}

void Core::Session::set_pitch(int pitch, bool monotone)
{
    if (handle_) {
        core_.p_set_pitch(handle_, pitch);
        core_.p_set_flat(handle_, monotone ? 1 : 0);
    }
}

void Core::Session::set_speed(double speed)
{
    if (handle_) {
        core_.p_set_speed(handle_, speed);
    }
}

}  // namespace EchoGPPC
