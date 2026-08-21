#pragma once

// Process-wide access to the echotalk emulator library.
//
// The library is a hard singleton: echotalk_create refuses a second instance
// with "an echotalk instance already exists (the 6502 core is a singleton)",
// because the emulated CPU keeps its registers in globals. SAPI5 engines are
// in-process COM objects and a host may well hold several voice objects at
// once, so one handle per engine object cannot work.
//
// Instead every engine object shares ONE machine behind a critical section.
// An utterance takes a Session, which locks the core and boots the requested
// ROM pair if a different one is currently loaded (booting costs well under
// 10 ms, so switching voices per utterance is affordable).

#include <string>
#include <vector>
#include <windows.h>

namespace EchoGPPC {

// One Textalker ROM pair, which is what SAPI sees as a voice.
struct VoiceRom {
    std::wstring stem;     // "textalker"
    std::wstring loader;   // full path to <stem>.ram.bin or <stem>.loader.bin
    std::wstring obj;      // full path to <stem>.obj.bin
    std::wstring banner;   // version read out of the ROM itself, e.g. "3.1.3"
    std::wstring display;  // "Echo GPPC Textalker 3.1.3"
};

// Everything the engine pushes into the library before an utterance.
struct CoreParams {
    int      pitch         = 24;
    int      volume        = 12;
    int      word_delay    = 0;
    int      repeat_filter = 99;
    double   speed         = 1.0;
    double   clock         = 1.0;
    unsigned sample_rate   = 22050;
    bool     monotone      = false;
    bool     compressed    = false;
    int      frame_rate    = 0;
    bool     raw           = false;
    int      letter_mode   = 0;
    int      punctuation   = 1;
    unsigned chunk_size    = 80;
    int      index_break   = 0;
};

class Core {
public:
    static Core& instance();

    // Loads echotalk32/64.dll and resolves every entry point. Idempotent.
    // On failure `error` gets a human-readable reason and false comes back;
    // the caller must cope rather than assume speech is available.
    bool ensure_loaded(std::wstring* error);

    [[nodiscard]] bool loaded() const noexcept { return lib_ != nullptr; }
    [[nodiscard]] unsigned abi_version() const noexcept { return abi_; }

    // Directory the ROM pairs and the emulator DLL were found in.
    [[nodiscard]] std::wstring media_directory();

    // Discovers and banner-probes every ROM pair exactly once, then caches.
    // Probing boots each machine briefly, so this takes the core lock.
    [[nodiscard]] std::vector<VoiceRom> voices();

    // The rate the chip really produces at `clock`. The requested output rate
    // is a floor, never a ceiling: asking for less would run real detail the
    // chip generated through a resampler with no anti-aliasing filter and
    // turn it into aliasing noise for no benefit.
    [[nodiscard]] static unsigned effective_sample_rate(unsigned requested, double clock) noexcept;

    // Locks the core for one utterance and guarantees the right ROM is booted.
    class Session {
    public:
        Session(Core& core, const VoiceRom& rom, const CoreParams& params);
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr; }
        [[nodiscard]] const std::wstring& error() const noexcept { return error_; }

        // The rate the library reports it is actually producing.
        [[nodiscard]] unsigned sample_rate() const noexcept;

        bool speak(const std::string& utf8_text);

        // Returns samples written into `buffer`; 0 means this utterance is done.
        [[nodiscard]] size_t read(short* buffer, size_t max_samples);

        // Drains index marks whose audio fell inside the block just read.
        [[nodiscard]] bool next_index(int* out);

        void stop();

        [[nodiscard]] unsigned overruns() const noexcept;
        [[nodiscard]] unsigned command_errors() const noexcept;

        // SAPI carries a rate, pitch and volume adjustment on every text
        // fragment, so all three have to be able to move part-way through an
        // utterance. The library takes them as live settings rather than as
        // inline text, which is simpler and cannot be mistaken for content.
        void set_pitch(int pitch, bool monotone);
        void set_speed(double speed);

    private:
        Core&        core_;
        void*        handle_ = nullptr;
        bool         locked_ = false;
        std::wstring error_;
    };

private:
    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    bool boot_locked(const VoiceRom& rom, std::wstring* error);
    void destroy_locked();
    void apply_locked(const CoreParams& params);

    HMODULE          lib_ = nullptr;
    unsigned         abi_ = 0;
    void*            handle_ = nullptr;
    std::wstring     loaded_stem_;
    std::wstring     media_dir_;
    std::vector<VoiceRom> voices_;
    bool             voices_scanned_ = false;

    CRITICAL_SECTION lock_{};

    // --- resolved entry points -------------------------------------------
    unsigned    (__cdecl* p_abi_version)(void)                       = nullptr;
    void*       (__cdecl* p_create)(const char*, const char*, char*, size_t) = nullptr;
    void        (__cdecl* p_destroy)(void*)                          = nullptr;
    const char* (__cdecl* p_version)(void*)                          = nullptr;
    int         (__cdecl* p_set_pitch)(void*, int)                   = nullptr;
    int         (__cdecl* p_set_flat)(void*, int)                    = nullptr;
    int         (__cdecl* p_set_volume)(void*, int)                  = nullptr;
    int         (__cdecl* p_set_word_delay)(void*, int)              = nullptr;
    int         (__cdecl* p_set_repeat_filter)(void*, int)           = nullptr;
    int         (__cdecl* p_set_compressed)(void*, int)              = nullptr;
    int         (__cdecl* p_set_frame_rate)(void*, int)              = nullptr;
    int         (__cdecl* p_set_raw)(void*, int)                     = nullptr;
    int         (__cdecl* p_set_letter_mode)(void*, int)             = nullptr;
    int         (__cdecl* p_set_punctuation)(void*, int)             = nullptr;
    int         (__cdecl* p_set_index_break)(void*, int)             = nullptr;
    int         (__cdecl* p_set_speed)(void*, double)                = nullptr;
    int         (__cdecl* p_set_clock_multiplier)(void*, double)     = nullptr;
    int         (__cdecl* p_set_sample_rate)(void*, unsigned)        = nullptr;
    int         (__cdecl* p_set_chunk_size)(void*, unsigned)         = nullptr;
    unsigned    (__cdecl* p_sample_rate)(void*)                      = nullptr;
    int         (__cdecl* p_speak)(void*, const char*)               = nullptr;
    size_t      (__cdecl* p_read)(void*, short*, size_t)             = nullptr;
    size_t      (__cdecl* p_available)(void*)                        = nullptr;
    size_t      (__cdecl* p_pending)(void*)                          = nullptr;
    int         (__cdecl* p_next_index)(void*, int*)                 = nullptr;
    void        (__cdecl* p_stop)(void*)                             = nullptr;
    unsigned    (__cdecl* p_overruns)(void*)                         = nullptr;
    unsigned    (__cdecl* p_command_errors)(void*)                   = nullptr;
    void        (__cdecl* p_clear_command_errors)(void*)             = nullptr;
};

// The ABI this engine was written against; a mismatch is refused rather than
// risked, because the failure mode of a wrong ABI is a corrupt audio pointer.
inline constexpr unsigned REQUIRED_ABI = 6;

}  // namespace EchoGPPC
