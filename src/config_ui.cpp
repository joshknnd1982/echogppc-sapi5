// Echo GPPC Configuration -- adjusts the SAPI5 engine's per-voice settings.
//
// Accessibility is the point of most of the decisions here, because the people
// most likely to want an emulated Echo II are screen reader users:
//
//  * Standard Win32 dialog controls throughout. Combo boxes, edit boxes with
//    spin buttons and check boxes all expose name, role and value through
//    MSAA with no help from us; an owner-drawn or custom control would not.
//  * Values are stored in the controls in the library's own units, so what a
//    screen reader reads out is the number that ends up in the INI file.
//    Nothing here shows a percentage that has to be mentally converted.
//  * Nothing is ever disabled, so nothing silently vanishes from the tab
//    order. The two Textalker-3-only fields say so in their labels.
//  * A status line reports what just happened -- saved, spoke, reset -- and is
//    announced, so the effect of a change is not purely audible in the voice.
//
// Voices are enumerated through SAPI rather than by loading the emulator, for
// two reasons: this utility then configures exactly the voices applications
// can actually see, and the emulator's one-machine-per-process limit is never
// at risk of being hit by this process holding an instance open while the
// engine tries to speak.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <sapi.h>
#include <sperror.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "config_resource.h"
#include "echo_settings.h"
#include "echo_log.h"

#pragma comment(linker,                                                       \
    "\"/manifestdependency:type='win32' "                                     \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "             \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "            \
    "language='*'\"")

using namespace EchoGPPC;

namespace {

// A settings write is deferred by this long so that holding down an arrow key
// on a spin control does not write the file once per repeat. Short enough that
// "immediate effect" still means immediate: the engine re-reads the file at the
// start of each utterance, and no one can arrow and then speak inside 250 ms.
constexpr UINT SAVE_DELAY_MS = 250;
constexpr UINT_PTR SAVE_TIMER_ID = 1;

struct EchoVoice {
    std::wstring stem;      // ROM stem, the INI section key
    std::wstring display;   // what SAPI calls it
};

// Multipliers offered for Rate and Chip clock. A curated list rather than a
// computed geometric series: these are the numbers a person would choose, and
// 1.00 is exactly representable and clearly marked.
const double MULTIPLIERS[] = {
    0.25, 0.30, 0.35, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90,
    1.00, 1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 3.50, 4.00,
};

const unsigned SAMPLE_RATES[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};

const wchar_t* const SAMPLE_RATE_LABELS[] = {
    L"8000 Hz (native)", L"11025 Hz", L"16000 Hz", L"22050 Hz",
    L"32000 Hz", L"44100 Hz", L"48000 Hz",
};

const wchar_t* const FRAME_RATE_LABELS[] = {
    L"0 - normal", L"1 - faster", L"2 - faster still", L"3 - fastest",
};

struct LogChoice {
    const wchar_t* label;
    log::Level     level;
};

const LogChoice LOG_CHOICES[] = {
    {L"Off",                      log::Level::Off},
    {L"Errors only",              log::Level::Error},
    {L"Errors and warnings",      log::Level::Warn},
    {L"Normal",                   log::Level::Info},
    {L"Detailed",                 log::Level::Debug},
    {L"Everything",               log::Level::Trace},
};

// --- state --------------------------------------------------------------

HINSTANCE              g_instance = nullptr;
std::vector<EchoVoice> g_voices;
Settings               g_settings;
std::wstring           g_ini_path;
size_t                 g_current = 0;      // index into g_voices
bool                   g_loading = false;  // suppresses change handlers
bool                   g_dirty = false;
ISpVoice*              g_speech = nullptr;

// --- helpers ------------------------------------------------------------

std::wstring format_multiplier(double value, const wchar_t* note)
{
    wchar_t buf[64];
    if (std::abs(value - 1.0) < 1e-9 && note) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.2fx (%ls)", value, note);
    } else {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.2fx", value);
    }
    return buf;
}

int nearest_multiplier(double value)
{
    int best = 0;
    double best_gap = 1e9;
    for (int i = 0; i < static_cast<int>(_countof(MULTIPLIERS)); ++i) {
        const double gap = std::abs(MULTIPLIERS[i] - value);
        if (gap < best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    return best;
}

int nearest_sample_rate(unsigned value)
{
    int best = 0;
    unsigned best_gap = 0xFFFFFFFFu;
    for (int i = 0; i < static_cast<int>(_countof(SAMPLE_RATES)); ++i) {
        const unsigned gap = SAMPLE_RATES[i] > value ? SAMPLE_RATES[i] - value
                                                     : value - SAMPLE_RATES[i];
        if (gap < best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    return best;
}

void set_status(HWND dlg, const wchar_t* text)
{
    SetDlgItemTextW(dlg, IDC_STATUS, text);
    // Nudge the accessibility layer so a screen reader reads the new text
    // rather than leaving the user to go looking for it.
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dlg, IDC_STATUS),
                   OBJID_CLIENT, CHILDID_SELF);
}

int get_int(HWND dlg, int id, int lo, int hi, int fallback)
{
    BOOL ok = FALSE;
    const int value = static_cast<int>(GetDlgItemInt(dlg, id, &ok, FALSE));
    if (!ok) {
        return fallback;
    }
    return std::clamp(value, lo, hi);
}

// --- voice discovery ----------------------------------------------------

std::wstring token_attribute(ISpObjectToken* token, const wchar_t* name)
{
    ISpDataKey* attrs = nullptr;
    if (FAILED(token->OpenKey(L"Attributes", &attrs)) || !attrs) {
        return {};
    }
    LPWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(attrs->GetStringValue(name, &raw)) && raw) {
        result = raw;
        CoTaskMemFree(raw);
    }
    attrs->Release();
    return result;
}

bool load_voices()
{
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&category)))) {
        return false;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) {
        category->Release();
        return false;
    }
    IEnumSpObjectTokens* tokens = nullptr;
    const HRESULT hr = category->EnumTokens(nullptr, nullptr, &tokens);
    category->Release();
    if (FAILED(hr)) {
        return false;
    }

    for (;;) {
        ISpObjectToken* token = nullptr;
        if (tokens->Next(1, &token, nullptr) != S_OK || !token) {
            break;
        }
        LPWSTR raw_stem = nullptr;
        // Only this engine's tokens carry EchoRomStem, so it doubles as the
        // filter and as the INI section key.
        if (SUCCEEDED(token->GetStringValue(L"EchoRomStem", &raw_stem)) && raw_stem) {
            EchoVoice voice;
            voice.stem = raw_stem;
            CoTaskMemFree(raw_stem);
            voice.display = token_attribute(token, L"Name");
            if (voice.display.empty()) {
                voice.display = voice.stem;
            }
            g_voices.push_back(voice);
        }
        token->Release();
    }
    tokens->Release();
    return !g_voices.empty();
}

// --- dialog -------------------------------------------------------------

void make_spin(HWND dlg, int edit_id, int spin_id, int lo, int hi)
{
    HWND edit = GetDlgItem(dlg, edit_id);
    // UDS_ARROWKEYS is what lets Up and Down adjust the value while focus is
    // in the edit box, which is how someone using a screen reader will do it:
    // the edit reads out its new value on every press.
    HWND spin = CreateWindowExW(
        0, UPDOWN_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
            UDS_ARROWKEYS | UDS_NOTHOUSANDS,
        0, 0, 0, 0, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(spin_id)),
        g_instance, nullptr);
    if (!spin) {
        return;
    }
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);
    SendMessageW(spin, UDM_SETRANGE32, static_cast<WPARAM>(lo),
                 static_cast<LPARAM>(hi));
    // Deliberately not a tab stop: it is a second way to drive the edit box,
    // not a field of its own, and stopping on it would mean two tab positions
    // announcing the same value.
    SetWindowLongPtrW(spin, GWL_STYLE,
                      GetWindowLongPtrW(spin, GWL_STYLE) & ~WS_TABSTOP);
}

void fill_combos(HWND dlg)
{
    for (const auto& voice : g_voices) {
        SendDlgItemMessageW(dlg, IDC_VOICE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(voice.display.c_str()));
    }
    for (double m : MULTIPLIERS) {
        SendDlgItemMessageW(dlg, IDC_RATE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(
                                format_multiplier(m, L"normal").c_str()));
        SendDlgItemMessageW(dlg, IDC_CLOCK, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(
                                format_multiplier(m, L"authentic").c_str()));
    }
    for (const wchar_t* label : SAMPLE_RATE_LABELS) {
        SendDlgItemMessageW(dlg, IDC_SAMPLERATE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(label));
    }
    for (const wchar_t* label : FRAME_RATE_LABELS) {
        SendDlgItemMessageW(dlg, IDC_FRAMERATE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(label));
    }
    for (const auto& choice : LOG_CHOICES) {
        SendDlgItemMessageW(dlg, IDC_LOGLEVEL, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(choice.label));
    }
}

void load_controls(HWND dlg)
{
    g_loading = true;

    const VoiceSettings vs = g_settings.for_voice(g_voices[g_current].stem);

    SendDlgItemMessageW(dlg, IDC_VOICE, CB_SETCURSEL,
                        static_cast<WPARAM>(g_current), 0);
    SendDlgItemMessageW(dlg, IDC_RATE, CB_SETCURSEL,
                        static_cast<WPARAM>(nearest_multiplier(vs.speed)), 0);
    SendDlgItemMessageW(dlg, IDC_CLOCK, CB_SETCURSEL,
                        static_cast<WPARAM>(nearest_multiplier(vs.clock)), 0);
    SendDlgItemMessageW(dlg, IDC_SAMPLERATE, CB_SETCURSEL,
                        static_cast<WPARAM>(nearest_sample_rate(vs.sample_rate)), 0);
    SendDlgItemMessageW(dlg, IDC_FRAMERATE, CB_SETCURSEL,
                        static_cast<WPARAM>(std::clamp(vs.frame_rate, 0, 3)), 0);

    SetDlgItemInt(dlg, IDC_PITCH, static_cast<UINT>(vs.pitch), FALSE);
    SetDlgItemInt(dlg, IDC_VOLUME, static_cast<UINT>(vs.volume), FALSE);
    SetDlgItemInt(dlg, IDC_WORDDELAY, static_cast<UINT>(vs.word_delay), FALSE);
    SetDlgItemInt(dlg, IDC_REPEAT, static_cast<UINT>(vs.repeat_filter), FALSE);

    CheckDlgButton(dlg, IDC_MONOTONE, vs.monotone ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_COMPRESSED, vs.compressed ? BST_CHECKED : BST_UNCHECKED);

    int log_index = 3;
    for (int i = 0; i < static_cast<int>(_countof(LOG_CHOICES)); ++i) {
        if (LOG_CHOICES[i].level == g_settings.log_level) {
            log_index = i;
            break;
        }
    }
    SendDlgItemMessageW(dlg, IDC_LOGLEVEL, CB_SETCURSEL,
                        static_cast<WPARAM>(log_index), 0);

    g_loading = false;
}

// Reads the controls back into g_settings for the voice being edited. The
// advanced keys the utility does not show are carried through untouched, so
// hand-editing the INI is not undone by opening this dialog.
void collect_controls(HWND dlg)
{
    const std::wstring& stem = g_voices[g_current].stem;
    VoiceSettings vs = g_settings.for_voice(stem);

    const int rate_index = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_RATE, CB_GETCURSEL, 0, 0));
    if (rate_index >= 0) {
        vs.speed = MULTIPLIERS[rate_index];
    }
    const int clock_index = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_CLOCK, CB_GETCURSEL, 0, 0));
    if (clock_index >= 0) {
        vs.clock = MULTIPLIERS[clock_index];
    }
    const int rate_sel = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_SAMPLERATE, CB_GETCURSEL, 0, 0));
    if (rate_sel >= 0) {
        vs.sample_rate = SAMPLE_RATES[rate_sel];
    }
    const int frame_sel = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_FRAMERATE, CB_GETCURSEL, 0, 0));
    if (frame_sel >= 0) {
        vs.frame_rate = frame_sel;
    }

    vs.pitch         = get_int(dlg, IDC_PITCH, 0, 63, vs.pitch);
    vs.volume        = get_int(dlg, IDC_VOLUME, 0, 15, vs.volume);
    vs.word_delay    = get_int(dlg, IDC_WORDDELAY, 0, 15, vs.word_delay);
    vs.repeat_filter = get_int(dlg, IDC_REPEAT, 0, 99, vs.repeat_filter);
    vs.monotone      = IsDlgButtonChecked(dlg, IDC_MONOTONE) == BST_CHECKED;
    vs.compressed    = IsDlgButtonChecked(dlg, IDC_COMPRESSED) == BST_CHECKED;

    g_settings.set_for_voice(stem, vs);
    g_settings.default_voice = stem;

    const int log_index = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_LOGLEVEL, CB_GETCURSEL, 0, 0));
    if (log_index >= 0) {
        g_settings.log_level = LOG_CHOICES[log_index].level;
    }
}

bool save_now(HWND dlg)
{
    collect_controls(dlg);
    KillTimer(dlg, SAVE_TIMER_ID);
    g_dirty = false;
    if (!save_settings(g_ini_path, g_settings)) {
        set_status(dlg, L"Could not save settings. Check permissions on the "
                        L"EchoGPPC folder in your profile.");
        return false;
    }
    log::set_level(g_settings.log_level);
    return true;
}

void schedule_save(HWND dlg)
{
    if (g_loading) {
        return;
    }
    g_dirty = true;
    SetTimer(dlg, SAVE_TIMER_ID, SAVE_DELAY_MS, nullptr);
}

void speak_test(HWND dlg)
{
    if (!save_now(dlg)) {
        return;
    }

    // A fresh voice object each time, because SAPI negotiates the audio format
    // once per voice: reusing one would keep speaking at the sample rate that
    // was configured when it was created, and the output-rate control would
    // look broken.
    if (g_speech) {
        g_speech->Release();
        g_speech = nullptr;
    }
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&g_speech)))) {
        set_status(dlg, L"Could not start SAPI to speak the test.");
        return;
    }

    ISpObjectTokenCategory* category = nullptr;
    ISpObjectToken* wanted = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                   IID_ISpObjectTokenCategory,
                                   reinterpret_cast<void**>(&category))) &&
        SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE))) {
        IEnumSpObjectTokens* tokens = nullptr;
        if (SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens))) {
            for (;;) {
                ISpObjectToken* token = nullptr;
                if (tokens->Next(1, &token, nullptr) != S_OK || !token) {
                    break;
                }
                LPWSTR raw = nullptr;
                if (SUCCEEDED(token->GetStringValue(L"EchoRomStem", &raw)) && raw) {
                    const bool match =
                        _wcsicmp(raw, g_voices[g_current].stem.c_str()) == 0;
                    CoTaskMemFree(raw);
                    if (match) {
                        wanted = token;
                        break;
                    }
                }
                token->Release();
            }
            tokens->Release();
        }
    }
    if (category) {
        category->Release();
    }

    if (!wanted) {
        set_status(dlg, L"That voice is not registered with SAPI any more.");
        return;
    }
    g_speech->SetVoice(wanted);
    wanted->Release();

    // Asynchronous, so the dialog stays responsive and a second press
    // interrupts the first rather than queueing behind it.
    const HRESULT hr = g_speech->Speak(
        L"The quick brown fox jumps over the lazy dog. "
        L"Zero one two three four five.",
        SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    set_status(dlg, FAILED(hr) ? L"The test could not be spoken."
                               : L"Speaking a test with the current settings.");
}

void reset_defaults(HWND dlg)
{
    g_settings.set_for_voice(g_voices[g_current].stem, VoiceSettings{});
    load_controls(dlg);
    if (save_now(dlg)) {
        set_status(dlg, L"This voice has been reset to its factory settings "
                        L"and saved.");
    }
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM /*lparam*/)
{
    switch (message) {
        case WM_INITDIALOG: {
            SendMessageW(dlg, WM_SETICON, ICON_BIG,
                         reinterpret_cast<LPARAM>(LoadIconW(
                             g_instance, MAKEINTRESOURCEW(IDI_APPICON))));
            SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(LoadIconW(
                             g_instance, MAKEINTRESOURCEW(IDI_APPICON))));

            make_spin(dlg, IDC_PITCH, IDC_PITCH_SPIN, 0, 63);
            make_spin(dlg, IDC_VOLUME, IDC_VOLUME_SPIN, 0, 15);
            make_spin(dlg, IDC_WORDDELAY, IDC_WORDDELAY_SPIN, 0, 15);
            make_spin(dlg, IDC_REPEAT, IDC_REPEAT_SPIN, 0, 99);

            fill_combos(dlg);

            // Start on whichever voice the INI names, so reopening the utility
            // lands where it was left.
            for (size_t i = 0; i < g_voices.size(); ++i) {
                if (_wcsicmp(g_voices[i].stem.c_str(),
                             g_settings.default_voice.c_str()) == 0) {
                    g_current = i;
                    break;
                }
            }
            load_controls(dlg);
            set_status(dlg, L"Changes are saved as you make them and take "
                            L"effect on the next thing spoken.");
            return TRUE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);

            switch (id) {
                case IDC_VOICE:
                    if (code == CBN_SELCHANGE && !g_loading) {
                        // Save the voice being left before switching, or its
                        // edits would be lost.
                        save_now(dlg);
                        const int sel = static_cast<int>(
                            SendDlgItemMessageW(dlg, IDC_VOICE, CB_GETCURSEL, 0, 0));
                        if (sel >= 0 && static_cast<size_t>(sel) < g_voices.size()) {
                            g_current = static_cast<size_t>(sel);
                            load_controls(dlg);
                            save_now(dlg);
                            set_status(dlg, L"Now editing the settings for this "
                                            L"voice.");
                        }
                    }
                    return TRUE;

                case IDC_RATE:
                case IDC_CLOCK:
                case IDC_SAMPLERATE:
                case IDC_FRAMERATE:
                case IDC_LOGLEVEL:
                    if (code == CBN_SELCHANGE) {
                        schedule_save(dlg);
                    }
                    return TRUE;

                case IDC_PITCH:
                case IDC_VOLUME:
                case IDC_WORDDELAY:
                case IDC_REPEAT:
                    if (code == EN_CHANGE) {
                        schedule_save(dlg);
                    }
                    return TRUE;

                case IDC_MONOTONE:
                case IDC_COMPRESSED:
                    schedule_save(dlg);
                    return TRUE;

                case IDC_SPEAK:
                    speak_test(dlg);
                    return TRUE;

                case IDC_RESET:
                    reset_defaults(dlg);
                    return TRUE;

                case IDC_OPENLOGS: {
                    const std::wstring dir = log::log_directory();
                    ShellExecuteW(dlg, L"open", dir.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    set_status(dlg, L"Opened the folder holding the diagnostic "
                                    L"logs.");
                    return TRUE;
                }

                case IDCANCEL:
                    if (g_dirty) {
                        save_now(dlg);
                    }
                    EndDialog(dlg, 0);
                    return TRUE;

                default:
                    break;
            }
            return FALSE;
        }

        case WM_TIMER:
            if (wparam == SAVE_TIMER_ID) {
                KillTimer(dlg, SAVE_TIMER_ID);
                if (g_dirty && save_now(dlg)) {
                    set_status(dlg, L"Saved. The change applies to the next "
                                    L"thing spoken.");
                }
                return TRUE;
            }
            return FALSE;

        case WM_CLOSE:
            if (g_dirty) {
                save_now(dlg);
            }
            EndDialog(dlg, 0);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    g_instance = instance;
    SetProcessDPIAware();

    log::init(L"config");
    g_ini_path = settings_path();
    if (!load_settings(g_ini_path, &g_settings)) {
        // No file yet: built-in defaults, which is exactly what a first run
        // should show.
        g_settings = Settings{};
    }
    log::set_level(g_settings.log_level);
    ECHO_INFO("Configuration utility starting, settings at %ls", g_ini_path.c_str());

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        MessageBoxW(nullptr, L"Windows would not start COM, so this utility "
                             L"cannot read the list of installed voices.",
                    L"Echo GPPC Configuration", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!load_voices()) {
        ECHO_ERR("No Echo GPPC voices are registered with SAPI");
        MessageBoxW(nullptr,
                    L"No Echo GPPC voices are registered on this computer, so "
                    L"there is nothing to configure.\n\n"
                    L"Reinstall Echo GPPC SAPI5, or register the engine by "
                    L"running, from an administrator command prompt:\n\n"
                    L"    regsvr32 EchoGPPCSAPI.dll",
                    L"Echo GPPC Configuration", MB_OK | MB_ICONWARNING);
        CoUninitialize();
        return 1;
    }
    ECHO_INFO("Configuration utility found %zu voice(s)", g_voices.size());

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), nullptr, dialog_proc, 0);

    if (g_speech) {
        g_speech->Release();
        g_speech = nullptr;
    }
    CoUninitialize();
    ECHO_INFO("Configuration utility closing");
    return 0;
}
