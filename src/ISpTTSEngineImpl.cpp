#include <new>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cwctype>
#include <algorithm>

#include "utils.hpp"
#include "ISpTTSEngineImpl.hpp"
#include "echo_log.h"

namespace EchoGPPC {
namespace sapi {

namespace {

constexpr WORD AUDIO_CHANNELS       = 1;
constexpr WORD AUDIO_BITS_PER_SAMPLE = 16;

// Samples pulled from the library per iteration. Emulation runs at roughly a
// hundred times real time, so this is not a throughput question -- it sets how
// quickly an abort is noticed and how precisely a word-boundary event can be
// placed, both of which are one block.
constexpr size_t READ_BLOCK       = 1024;
constexpr size_t READ_BLOCK_FINE  = 256;   // when the host wants word events

// SAPI's rate is -10..+10. Two octaves either side of normal matches the
// library's own 0.25x..4x speed range and puts 1.0x exactly at 0.
constexpr double RATE_STEPS_PER_OCTAVE = 5.0;

constexpr int PITCH_MAX = 63;

// SAPI's per-fragment pitch adjustment is nominally -10..+10. Textalker has
// 63 pitch steps, so a little over two steps per unit spans a useful range
// without slamming into either end at ±10.
constexpr double PITCH_STEPS_PER_UNIT = 2.0;

// Bytes the pipeline would act on rather than speak: Ctrl-D introduces a
// driver command, Ctrl-E a Textalker command and Ctrl-V phoneme mode. They
// have to come out of anything that came off the screen, or a document that
// happens to contain one silently changes the voice. Everything else is left
// alone so the library's own text preparation can fold UTF-8 and typographic
// punctuation, which it does better than this engine could.
[[nodiscard]] std::wstring strip_control(const wchar_t* text, size_t len)
{
    std::wstring out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const wchar_t c = text[i];
        out.push_back((c == 0x04 || c == 0x05 || c == 0x16) ? L' ' : c);
    }
    return out;
}

[[nodiscard]] bool is_word_char(wchar_t c)
{
    return iswalnum(c) || c == L'\'' || c == L'-';
}

struct WordSpan {
    ULONG src_offset;
    ULONG length;
};

// Applies volume as a clean digital gain rather than through Textalker's own
// volume register. The register is kept for the "character" the utility
// exposes -- low settings are meant to sound fuzzy and high ones distorted --
// so mapping the host's volume onto it too would make one control fight the
// other and leave no way to get clean quiet speech.
void apply_gain(short* samples, size_t count, unsigned gain_q16)
{
    if (gain_q16 == 65536) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const int scaled = static_cast<int>(
            (static_cast<long long>(samples[i]) * gain_q16) >> 16);
        samples[i] = static_cast<short>(std::clamp(scaled, -32768, 32767));
    }
}

}  // namespace

SettingsWatcher& settings_watcher()
{
    static SettingsWatcher watcher;
    return watcher;
}

ISpTTSEngineImpl::ISpTTSEngineImpl() = default;
ISpTTSEngineImpl::~ISpTTSEngineImpl() = default;

VoiceSettings ISpTTSEngineImpl::current_settings()
{
    // One stat of the INI per utterance. When it moved, the file is re-parsed
    // and the new values apply from this utterance on -- which is what makes a
    // change in the configuration utility take effect without restarting the
    // host application.
    if (settings_watcher().refresh()) {
        ECHO_INFO("Settings reloaded from %ls", settings_path().c_str());
    }
    const Settings all = settings_watcher().snapshot();
    return all.for_voice(rom_valid_ ? rom_.stem : std::wstring());
}

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }

    try {
        ISpDataKeyPtr attr;
        if (FAILED(pToken->OpenKey(L"Attributes", &attr))) {
            ECHO_ERR("SetObjectToken: token has no Attributes key");
            return E_INVALIDARG;
        }

        utils::out_ptr<wchar_t> name(CoTaskMemFree);
        if (FAILED(attr->GetStringValue(L"Name", name.address()))) {
            ECHO_ERR("SetObjectToken: token has no Name attribute");
            return E_INVALIDARG;
        }
        const std::wstring voice_name(name.get() ? name.get() : L"");

        // The ROM stem is stored on the token by voice_token, so prefer it and
        // only fall back to matching on the display name -- which is what a
        // token restored from a saved registry entry would carry.
        std::wstring stem;
        utils::out_ptr<wchar_t> stem_value(CoTaskMemFree);
        if (SUCCEEDED(pToken->GetStringValue(L"EchoRomStem", stem_value.address())) &&
            stem_value.get()) {
            stem = stem_value.get();
        }

        const std::vector<VoiceRom> voices = Core::instance().voices();
        if (voices.empty()) {
            ECHO_ERR("SetObjectToken: no Echo GPPC ROM pairs available");
            return SPERR_NOT_FOUND;
        }

        rom_valid_ = false;
        for (const VoiceRom& rom : voices) {
            if ((!stem.empty() && _wcsicmp(rom.stem.c_str(), stem.c_str()) == 0) ||
                _wcsicmp(rom.display.c_str(), voice_name.c_str()) == 0) {
                rom_ = rom;
                rom_valid_ = true;
                break;
            }
        }
        if (!rom_valid_) {
            // Speaking with the wrong voice beats not speaking at all: a
            // screen reader that goes quiet is the worst outcome here.
            rom_ = voices.front();
            rom_valid_ = true;
            ECHO_WARN("SetObjectToken: voice '%ls' (stem '%ls') not found; "
                      "falling back to '%ls'",
                      voice_name.c_str(), stem.c_str(), rom_.display.c_str());
        }

        token_ = pToken;
        ECHO_INFO("SetObjectToken: voice '%ls' -> ROM '%ls'",
                  rom_.display.c_str(), rom_.stem.c_str());
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        ECHO_ERR("SetObjectToken: unexpected exception");
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;

    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(
    const GUID* /*pTargetFmtId*/,
    const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pOutputFormatId,
    WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }
    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = nullptr;

    const VoiceSettings vs = current_settings();
    // The chip really produces 8000 * clock. Handing SAPI a lower rate would
    // mean downsampling through a resampler with no anti-aliasing filter,
    // folding real detail back into the audible band as noise -- so the
    // configured rate is a floor that the clock can raise.
    const unsigned rate = Core::effective_sample_rate(vs.sample_rate, vs.clock);
    negotiated_rate_.store(rate, std::memory_order_relaxed);

    auto* pwfex = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!pwfex) {
        return E_OUTOFMEMORY;
    }
    pwfex->wFormatTag      = WAVE_FORMAT_PCM;
    pwfex->nChannels       = AUDIO_CHANNELS;
    pwfex->nSamplesPerSec  = rate;
    pwfex->wBitsPerSample  = AUDIO_BITS_PER_SAMPLE;
    pwfex->nBlockAlign     = pwfex->nChannels * pwfex->wBitsPerSample / 8;
    pwfex->nAvgBytesPerSec = pwfex->nSamplesPerSec * pwfex->nBlockAlign;
    pwfex->cbSize          = 0;

    *ppCoMemOutputWaveFormatEx = pwfex;
    ECHO_INFO("GetOutputFormat: 16-bit mono PCM at %u Hz "
              "(setting %u Hz, chip clock %.3fx)", rate, vs.sample_rate, vs.clock);
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(
    DWORD dwSpeakFlags,
    REFGUID /*rguidFormatId*/,
    const WAVEFORMATEX* /*pWaveFormatEx*/,
    const SPVTEXTFRAG* pTextFragList,
    ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }

    try {
        if (!rom_valid_) {
            // A host is allowed to Speak without ever calling SetObjectToken.
            const std::vector<VoiceRom> voices = Core::instance().voices();
            if (voices.empty()) {
                ECHO_ERR("Speak: no Echo GPPC voices available");
                return SPERR_NOT_FOUND;
            }
            rom_ = voices.front();
            rom_valid_ = true;
            ECHO_WARN("Speak: no token was set; using '%ls'", rom_.display.c_str());
        }

        const VoiceSettings vs = current_settings();

        long sapi_rate = 0;
        pOutputSite->GetRate(&sapi_rate);
        USHORT sapi_volume = 100;
        pOutputSite->GetVolume(&sapi_volume);

        ULONGLONG event_interest = 0;
        pOutputSite->GetEventInterest(&event_interest);
        const bool want_sentence = (event_interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        const bool want_word     = (event_interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_bookmark = (event_interest & SPFEI(SPEI_TTS_BOOKMARK)) != 0;

        const unsigned rate = negotiated_rate_.load(std::memory_order_relaxed);

        CoreParams params;
        params.pitch         = vs.pitch;
        params.volume        = vs.volume;
        params.word_delay    = vs.word_delay;
        params.repeat_filter = vs.repeat_filter;
        params.clock         = vs.clock;
        params.monotone      = vs.monotone;
        params.compressed    = vs.compressed;
        params.frame_rate    = vs.frame_rate;
        params.raw           = vs.raw;
        params.letter_mode   = vs.letter_mode;
        params.punctuation   = vs.punctuation;
        params.chunk_size    = vs.chunk_size;
        params.index_break   = vs.index_break;
        // Audio must keep coming out at the rate SAPI was promised, so the
        // negotiated value wins over the configured one for this stream. A
        // changed setting takes effect the next time a host asks for a format.
        params.sample_rate   = rate;

        const double base_speed = vs.speed;
        double speed = std::clamp(
            base_speed * std::pow(2.0, static_cast<double>(sapi_rate) / RATE_STEPS_PER_OCTAVE),
            0.25, 4.0);
        params.speed = speed;

        ECHO_INFO("Speak: voice='%ls' flags=0x%08lX rate=%ld vol=%u -> speed %.3fx "
                  "(base %.3fx), pitch %d, clock %.3fx, %u Hz",
                  rom_.display.c_str(), dwSpeakFlags, sapi_rate, sapi_volume,
                  speed, base_speed, vs.pitch, vs.clock, rate);

        Core::Session session(Core::instance(), rom_, params);
        if (!session.ok()) {
            ECHO_ERR("Speak: could not open the emulator: %ls", session.error().c_str());
            return E_FAIL;
        }

        const unsigned actual = session.sample_rate();
        if (actual != rate) {
            // Writing at a rate SAPI is not expecting produces speech at the
            // wrong pitch, so say so loudly rather than let it pass.
            ECHO_WARN("Speak: library is producing %u Hz but SAPI expects %u Hz; "
                      "the new output rate applies to the next stream", actual, rate);
        }

        unsigned gain_q16 = static_cast<unsigned>(
            (std::min<unsigned>(sapi_volume, 100) * 65536u) / 100u);

        ULONGLONG bytes_written = 0;
        std::vector<short> buffer;
        bool aborted = false;

        for (const SPVTEXTFRAG* frag = pTextFragList; frag && !aborted; frag = frag->pNext) {
            DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                ECHO_DEBUG("Speak: abort between fragments");
                break;
            }
            if (actions & SPVES_SKIP) {
                pOutputSite->CompleteSkip(0);
                ECHO_DEBUG("Speak: skip between fragments");
                break;
            }
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&sapi_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&sapi_volume);
                gain_q16 = static_cast<unsigned>(
                    (std::min<unsigned>(sapi_volume, 100) * 65536u) / 100u);
            }

            if (frag->State.eAction == SPVA_Bookmark) {
                if (!want_bookmark) {
                    continue;
                }
                // The offset is exact: every preceding fragment has already
                // been rendered and written by the time this is reached.
                std::wstring text = (frag->ulTextLen && frag->pTextStart)
                                        ? std::wstring(frag->pTextStart, frag->ulTextLen)
                                        : std::wstring();
                long id = 0;
                if (!text.empty()) {
                    id = wcstol(text.c_str(), nullptr, 10);
                }
                SPEVENT ev{};
                ev.eEventId            = SPEI_TTS_BOOKMARK;
                ev.elParamType         = text.empty() ? SPET_LPARAM_IS_UNDEFINED
                                                      : SPET_LPARAM_IS_STRING;
                ev.ullAudioStreamOffset = bytes_written;
                ev.lParam              = text.empty()
                                            ? 0
                                            : reinterpret_cast<LPARAM>(text.c_str());
                ev.wParam              = static_cast<WPARAM>(id);
                const HRESULT ev_hr = pOutputSite->AddEvents(&ev, 1);
                ECHO_DEBUG("Speak: bookmark '%ls' (id %ld) at byte %llu, AddEvents=0x%08lX",
                           text.c_str(), id, bytes_written, ev_hr);
                continue;
            }

            if (frag->State.eAction != SPVA_Speak && frag->State.eAction != SPVA_SpellOut) {
                continue;
            }
            if (!frag->ulTextLen || !frag->pTextStart) {
                continue;
            }

            std::wstring clean = strip_control(frag->pTextStart, frag->ulTextLen);
            if (clean.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                continue;
            }

            // Per-fragment prosody. RateAdj and PitchAdj ride on top of the
            // whole-utterance values rather than replacing them.
            const double frag_speed = std::clamp(
                base_speed * std::pow(2.0,
                    static_cast<double>(std::clamp<long>(sapi_rate + frag->State.RateAdj, -10, 10))
                        / RATE_STEPS_PER_OCTAVE),
                0.25, 4.0);
            if (std::abs(frag_speed - speed) > 1e-9) {
                session.set_speed(frag_speed);
                speed = frag_speed;
            }

            const int frag_pitch = std::clamp(
                vs.pitch + static_cast<int>(std::lround(
                    frag->State.PitchAdj.MiddleAdj * PITCH_STEPS_PER_UNIT)),
                0, PITCH_MAX);
            session.set_pitch(frag_pitch, vs.monotone);

            // SAPI's own per-fragment volume multiplies the host's.
            const unsigned frag_gain = static_cast<unsigned>(
                (static_cast<unsigned long long>(gain_q16) *
                 std::min<unsigned>(frag->State.Volume, 100)) / 100u);

            if (want_sentence) {
                SPEVENT ev{};
                ev.eEventId            = SPEI_SENTENCE_BOUNDARY;
                ev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
                ev.ullAudioStreamOffset = bytes_written;
                ev.lParam              = frag->ulTextSrcOffset;
                ev.wParam              = frag->ulTextLen;
                pOutputSite->AddEvents(&ev, 1);
            }

            // Word-boundary events are placed using the library's own index
            // marks: a \x04<n>I in the text comes back out of next_index()
            // when the audio for that point is produced, which is real timing
            // rather than a guess. Only done when a host asks for the events,
            // since the markers do lengthen the text Textalker has to chew on.
            std::map<int, WordSpan> marks;
            std::wstring to_speak;
            if (want_word) {
                to_speak.reserve(clean.size() + 16);
                int next_id = 1;
                size_t i = 0;
                while (i < clean.size()) {
                    if (!is_word_char(clean[i])) {
                        to_speak.push_back(clean[i]);
                        ++i;
                        continue;
                    }
                    const size_t start = i;
                    while (i < clean.size() && is_word_char(clean[i])) {
                        ++i;
                    }
                    const int id = next_id++;
                    marks[id] = WordSpan{
                        frag->ulTextSrcOffset + static_cast<ULONG>(start),
                        static_cast<ULONG>(i - start)};
                    wchar_t cmd[16];
                    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\x04%dI", id);
                    to_speak += cmd;
                    to_speak.append(clean, start, i - start);
                }
            } else if (frag->State.eAction == SPVA_SpellOut) {
                // Textalker's own letter mode is inert in this ABI, so spell
                // by spacing the characters out, which it does pronounce
                // individually.
                for (wchar_t c : clean) {
                    to_speak.push_back(c);
                    to_speak.push_back(L' ');
                }
            } else {
                to_speak = clean;
            }

            const std::string utf8 = utils::wstring_to_string(to_speak);
            if (utf8.empty()) {
                continue;
            }
            ECHO_DEBUG("Speak: fragment %zu chars, %zu word mark(s), pitch %d, speed %.3fx",
                       clean.size(), marks.size(), frag_pitch, frag_speed);

            if (!session.speak(utf8)) {
                ECHO_ERR("Speak: the emulator rejected the text");
                continue;
            }

            const size_t block = want_word ? READ_BLOCK_FINE : READ_BLOCK;
            buffer.resize(block);

            for (;;) {
                actions = pOutputSite->GetActions();
                if (actions & SPVES_ABORT) {
                    session.stop();
                    aborted = true;
                    ECHO_DEBUG("Speak: abort during synthesis");
                    break;
                }
                if (actions & SPVES_SKIP) {
                    session.stop();
                    pOutputSite->CompleteSkip(0);
                    aborted = true;
                    ECHO_DEBUG("Speak: skip during synthesis");
                    break;
                }

                const size_t got = session.read(buffer.data(), block);

                // Drain after EVERY read including the one returning 0: a mark
                // at the very end of the text only becomes ready then.
                int id = 0;
                std::vector<SPEVENT> events;
                while (session.next_index(&id)) {
                    const auto it = marks.find(id);
                    if (it == marks.end()) {
                        continue;
                    }
                    SPEVENT ev{};
                    ev.eEventId            = SPEI_WORD_BOUNDARY;
                    ev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
                    ev.ullAudioStreamOffset = bytes_written;
                    ev.lParam              = it->second.src_offset;
                    ev.wParam              = it->second.length;
                    events.push_back(ev);
                }
                if (!events.empty()) {
                    const HRESULT ev_hr = pOutputSite->AddEvents(
                        events.data(), static_cast<ULONG>(events.size()));
                    ECHO_DEBUG("Speak: %zu word-boundary event(s) at byte %llu, "
                               "AddEvents=0x%08lX", events.size(), bytes_written, ev_hr);
                }

                if (got == 0) {
                    break;
                }

                apply_gain(buffer.data(), got, frag_gain);

                const BYTE* ptr = reinterpret_cast<const BYTE*>(buffer.data());
                ULONG remaining = static_cast<ULONG>(got * sizeof(short));
                while (remaining > 0) {
                    ULONG written = 0;
                    const HRESULT hr = pOutputSite->Write(ptr, remaining, &written);
                    if (FAILED(hr)) {
                        ECHO_ERR("Speak: site Write failed, hr=0x%08lX", hr);
                        session.stop();
                        return hr;
                    }
                    if (written == 0 || written > remaining) {
                        ECHO_ERR("Speak: site Write returned %lu of %lu bytes",
                                 written, remaining);
                        session.stop();
                        return E_FAIL;
                    }
                    bytes_written += written;
                    remaining -= written;
                    ptr += written;
                }
            }
        }

        // A runaway guard tripping means the 6502 was cut off part-way through
        // a routine and the speech just produced is wrong. It is silent on its
        // own, so it goes in the log: silence is what makes this kind of fault
        // so hard to pin down from a bug report.
        const unsigned over = session.overruns();
        const unsigned cmd_errors = session.command_errors();
        if (over) {
            ECHO_ERR("Speak: %u emulation overrun(s); the speech just produced is "
                     "wrong. Please report the settings in use.", over);
        }
        if (cmd_errors) {
            ECHO_WARN("Speak: %u Textalker command error(s) -- a command in the text "
                      "was not understood by this ROM version", cmd_errors);
        }

        ECHO_INFO("Speak: done, %llu bytes written%ls", bytes_written,
                  aborted ? L" (cut short)" : L"");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        ECHO_ERR("Speak: out of memory");
        return E_OUTOFMEMORY;
    }
    catch (...) {
        ECHO_ERR("Speak: unexpected exception");
        return E_UNEXPECTED;
    }
}

}
}
