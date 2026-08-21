#pragma once

// Persistent settings for the Echo GPPC SAPI5 engines.
//
// Deliberately a plain INI file under %APPDATA%, not the registry: the engine
// must be able to speak without reading a single registry key, and a per-user
// file also means the configuration utility needs no elevation to save.
// (The registry is still used for COM registration at install time -- that is
// SAPI's own discovery mechanism and there is no way around it.)
//
// The file is re-read whenever its timestamp or size moves, which is what
// makes a change in the utility take effect on the next utterance without
// restarting the host.

#include <map>
#include <string>
#include <windows.h>

#include "echo_log.h"

namespace EchoGPPC {

// Every tunable the echotalk ABI exposes, in the library's own units.
struct VoiceSettings {
    int      pitch         = 24;     // 0..63   Textalker nP
    int      volume        = 12;     // 0..15   Textalker nV
    int      word_delay    = 0;      // 0..15   Textalker nD (version 3 only)
    int      repeat_filter = 99;     // 0..99   Textalker nR (version 3 only)
    double   speed         = 1.0;    // 0.25..4.0 continuous, pitch-preserving
    double   clock         = 1.0;    // 0.25..4.0 TMS5220 clock: speed AND pitch
    unsigned sample_rate   = 22050;  // a floor; the chip clock can raise it
    bool     monotone      = false;  // Textalker nF instead of nP
    bool     compressed    = false;  // Textalker's own compressed/expanded
    int      frame_rate    = 0;      // 0..3    the chip's own four-step rate

    // Advanced. `raw` is the only one of these that measurably changes the
    // audio in ABI 6: with raw off the library folds UTF-8 and typographic
    // punctuation into something Textalker can pronounce, which is what a
    // screen reader wants. letter_mode and punctuation are accepted by the
    // library but produced byte-identical audio in every case tested, so they
    // are kept configurable and left out of the utility rather than shown as
    // controls that do nothing.
    bool     raw           = false;
    int      letter_mode   = 0;      // 0..1
    int      punctuation   = 1;      // 0..2
    unsigned chunk_size    = 80;     // Textalker's text buffer is 80 bytes
    int      index_break   = 0;      // 0..1

    void clamp() noexcept;
};

struct Settings {
    std::wstring                        default_voice;   // ROM stem
    log::Level                          log_level = log::Level::Info;
    std::map<std::wstring, VoiceSettings> voices;        // keyed by ROM stem

    // Settings for `stem`, falling back to defaults for a voice the file has
    // never seen. Never fails: an unknown voice must still speak.
    [[nodiscard]] VoiceSettings for_voice(const std::wstring& stem) const;
    void set_for_voice(const std::wstring& stem, const VoiceSettings& value);
};

// Full path of the settings file, creating its directory if needed.
[[nodiscard]] std::wstring settings_path();

[[nodiscard]] bool load_settings(const std::wstring& path, Settings* out);
[[nodiscard]] bool save_settings(const std::wstring& path, const Settings& in);

// Watches the settings file and hands out the current values.
//
// The engine holds one of these. `refresh()` is called at the top of every
// utterance; it stats the file and only re-parses when something moved, so
// the common case costs one GetFileAttributesEx.
class SettingsWatcher {
public:
    SettingsWatcher();
    ~SettingsWatcher();

    SettingsWatcher(const SettingsWatcher&) = delete;
    SettingsWatcher& operator=(const SettingsWatcher&) = delete;

    // Returns true when the file had changed and the values were reloaded.
    bool refresh();

    [[nodiscard]] Settings snapshot();

private:
    CRITICAL_SECTION lock_{};
    std::wstring     path_;
    Settings         settings_;
    FILETIME         stamp_{};
    ULONGLONG        size_ = 0;
    bool             loaded_ = false;
};

}  // namespace EchoGPPC
