// End-to-end check of the Echo GPPC SAPI5 engine.
//
// Everything here goes through the real SAPI5 stack -- CoCreateInstance,
// ISpVoice, token enumeration -- rather than calling the engine directly, so a
// pass means a SAPI application really can speak with these voices, not merely
// that the code compiles.
//
//   EchoGPPCTest --list
//   EchoGPPCTest --wav <directory> [--say "text"]
//   EchoGPPCTest --speak [--say "text"]          (to the sound card)
//   EchoGPPCTest --stress [N]                    (all voices at once)
//
// Add --rate N (-10..10) and --volume N (0..100) to exercise the host-side
// controls, and --bookmarks to check event timing (needs --speak; see below).

#include <windows.h>
#include <sapi.h>
#include <sperror.h>
#include <comdef.h>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

namespace {

struct VoiceEntry {
    ISpObjectToken* token = nullptr;
    std::wstring    name;
    std::wstring    stem;
};

int g_failures = 0;

void fail(const wchar_t* what, HRESULT hr)
{
    wprintf(L"  FAIL: %ls (hr=0x%08lX)\n", what, static_cast<unsigned long>(hr));
    ++g_failures;
}

std::wstring token_string(ISpObjectToken* token, const wchar_t* value)
{
    LPWSTR raw = nullptr;
    if (FAILED(token->GetStringValue(value, &raw)) || !raw) {
        return {};
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

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

// Collects every Echo GPPC voice SAPI can see. Enumerating the whole category
// (rather than looking our own enumerator up directly) is the point: it proves
// the voices are reachable the way any application would reach them.
std::vector<VoiceEntry> find_echo_voices(bool list_all)
{
    std::vector<VoiceEntry> found;

    ISpObjectTokenCategory* category = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                  IID_ISpObjectTokenCategory,
                                  reinterpret_cast<void**>(&category));
    if (FAILED(hr)) {
        fail(L"CoCreateInstance(SpObjectTokenCategory)", hr);
        return found;
    }
    hr = category->SetId(SPCAT_VOICES, FALSE);
    if (FAILED(hr)) {
        fail(L"ISpObjectTokenCategory::SetId(SPCAT_VOICES)", hr);
        category->Release();
        return found;
    }

    IEnumSpObjectTokens* tokens = nullptr;
    hr = category->EnumTokens(nullptr, nullptr, &tokens);
    category->Release();
    if (FAILED(hr)) {
        fail(L"IEnumSpObjectTokens", hr);
        return found;
    }

    ULONG count = 0;
    tokens->GetCount(&count);
    if (list_all) {
        wprintf(L"SAPI5 reports %lu voice(s) installed:\n", count);
    }

    for (;;) {
        ISpObjectToken* token = nullptr;
        if (tokens->Next(1, &token, nullptr) != S_OK || !token) {
            break;
        }
        const std::wstring name = token_attribute(token, L"Name");
        const std::wstring vendor = token_attribute(token, L"Vendor");
        const std::wstring language = token_attribute(token, L"Language");
        const std::wstring stem = token_string(token, L"EchoRomStem");

        const bool is_echo = name.find(L"Echo GPPC") != std::wstring::npos;
        if (list_all) {
            wprintf(L"  %-42ls vendor=%-32ls lang=%-5ls%ls\n",
                    name.c_str(), vendor.c_str(), language.c_str(),
                    is_echo ? L"  <-- Echo GPPC" : L"");
        }
        if (is_echo) {
            VoiceEntry entry;
            entry.token = token;
            entry.name = name;
            entry.stem = stem;
            found.push_back(entry);
            continue;  // keep the reference
        }
        token->Release();
    }
    tokens->Release();
    return found;
}

// Binds the voice's output to a wave file. The format is asked of the voice
// itself so the file matches whatever the engine negotiated, rather than
// silently going through SAPI's resampler.
ISpStream* make_wav_stream(const std::wstring& path, ISpVoice* voice)
{
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 22050;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    ISpStreamFormat* existing = nullptr;
    if (voice && SUCCEEDED(voice->GetOutputStream(&existing)) && existing) {
        GUID fmt{};
        WAVEFORMATEX* negotiated = nullptr;
        if (SUCCEEDED(existing->GetFormat(&fmt, &negotiated)) && negotiated) {
            wfx = *negotiated;
            CoTaskMemFree(negotiated);
        }
        existing->Release();
    }

    ISpStream* stream = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                                  reinterpret_cast<void**>(&stream));
    if (FAILED(hr)) {
        fail(L"CoCreateInstance(SpStream)", hr);
        return nullptr;
    }
    hr = stream->BindToFile(path.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx,
                            &wfx, SPFEI_ALL_EVENTS);
    if (FAILED(hr)) {
        fail(L"ISpStream::BindToFile", hr);
        stream->Release();
        return nullptr;
    }
    return stream;
}

// SpClearEvent lives in sphelper.h, which does not compile cleanly against
// modern toolchains. Only two parameter types own anything, so freeing them by
// hand keeps this harness free of that header.
void clear_event(SPEVENT* ev)
{
    if (ev->elParamType == SPET_LPARAM_IS_STRING) {
        CoTaskMemFree(reinterpret_cast<void*>(ev->lParam));
    } else if (ev->elParamType == SPET_LPARAM_IS_TOKEN ||
               ev->elParamType == SPET_LPARAM_IS_OBJECT) {
        if (ev->lParam) {
            reinterpret_cast<IUnknown*>(ev->lParam)->Release();
        }
    }
    *ev = SPEVENT{};
}

long long file_size(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return -1;
    }
    return (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

// --- concurrency ---------------------------------------------------------

struct StressArgs {
    ISpObjectToken* token = nullptr;
    const wchar_t*  text = nullptr;
    int             iterations = 0;
    int             failures = 0;
};

// One thread per voice, all speaking at the same time in one process.
//
// This is the case the emulator's singleton 6502 makes dangerous: every voice
// object in the process shares one machine, so if the engine's locking is
// wrong this either deadlocks, fails outright, or -- worst, because it is
// silent -- interleaves two voices into one stream of audio.
DWORD WINAPI stress_thread(LPVOID param)
{
    auto* args = static_cast<StressArgs*>(param);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        ++args->failures;
        return 1;
    }

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);

    for (int i = 0; i < args->iterations; ++i) {
        ISpVoice* voice = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                    reinterpret_cast<void**>(&voice)))) {
            ++args->failures;
            break;
        }

        wchar_t path[MAX_PATH];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"%lsecho_stress_%lu_%d.wav",
                     temp, GetCurrentThreadId(), i);

        if (SUCCEEDED(voice->SetVoice(args->token))) {
            ISpStream* stream = make_wav_stream(path, voice);
            if (stream && SUCCEEDED(voice->SetOutput(stream, TRUE))) {
                if (FAILED(voice->Speak(args->text, SPF_DEFAULT, nullptr)) ||
                    FAILED(voice->WaitUntilDone(60000))) {
                    ++args->failures;
                }
                voice->SetOutput(nullptr, FALSE);
                stream->Close();
                // Under 1 KB means the utterance produced no audio, which is
                // what a lock that let two voices collide would look like.
                if (file_size(path) < 1024) {
                    ++args->failures;
                }
            } else {
                ++args->failures;
            }
            if (stream) {
                stream->Release();
            }
        } else {
            ++args->failures;
        }

        DeleteFileW(path);
        voice->Release();
    }

    CoUninitialize();
    return 0;
}

void run_stress(std::vector<VoiceEntry>& voices, int iterations)
{
    wprintf(L"\n=== concurrency: %zu voice(s) x %d utterance(s), all at once ===\n",
            voices.size(), iterations);

    std::vector<StressArgs> args(voices.size());
    for (size_t i = 0; i < voices.size(); ++i) {
        args[i].token = voices[i].token;
        args[i].text = L"The quick brown fox jumps over the lazy dog.";
        args[i].iterations = iterations;
    }

    std::vector<HANDLE> threads;
    for (auto& a : args) {
        HANDLE t = CreateThread(nullptr, 0, stress_thread, &a, 0, nullptr);
        if (t) {
            threads.push_back(t);
        } else {
            ++a.failures;
        }
    }

    if (!threads.empty()) {
        // A deadlock in the shared-machine lock shows up here as a timeout
        // rather than as a hang somebody has to kill by hand.
        const DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(threads.size()),
                                                    threads.data(), TRUE, 120000);
        if (waited == WAIT_TIMEOUT) {
            wprintf(L"  FAIL: threads did not finish within 120 s -- deadlock\n");
            ++g_failures;
        }
        for (HANDLE t : threads) {
            CloseHandle(t);
        }
    }

    int total = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        wprintf(L"  %-34ls %d failure(s)\n", voices[i].name.c_str(), args[i].failures);
        total += args[i].failures;
    }
    if (total) {
        g_failures += total;
    } else {
        wprintf(L"  all %d concurrent utterances produced audio\n",
                static_cast<int>(voices.size()) * iterations);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    bool list_only = false;
    bool to_speaker = false;
    bool bookmarks = false;
    int  stress = 0;
    std::wstring wav_dir;
    std::wstring text = L"Hello. This is the Echo, from Street Electronics, "
                        L"speaking through SAPI 5.";
    long rate = 0;
    unsigned volume = 100;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--list") {
            list_only = true;
        } else if (arg == L"--speak") {
            to_speaker = true;
        } else if (arg == L"--bookmarks") {
            bookmarks = true;
        } else if (arg == L"--stress") {
            stress = (i + 1 < argc && iswdigit(argv[i + 1][0]))
                         ? static_cast<int>(wcstol(argv[++i], nullptr, 10))
                         : 4;
        } else if (arg == L"--wav" && i + 1 < argc) {
            wav_dir = argv[++i];
        } else if (arg == L"--say" && i + 1 < argc) {
            text = argv[++i];
        } else if (arg == L"--rate" && i + 1 < argc) {
            rate = wcstol(argv[++i], nullptr, 10);
        } else if (arg == L"--volume" && i + 1 < argc) {
            volume = static_cast<unsigned>(wcstoul(argv[++i], nullptr, 10));
        } else {
            wprintf(L"Unknown argument: %ls\n", arg.c_str());
            return 2;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        wprintf(L"CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    wprintf(L"Echo GPPC SAPI5 self-test (%d-bit)\n\n", static_cast<int>(sizeof(void*) * 8));

    std::vector<VoiceEntry> voices = find_echo_voices(true);
    wprintf(L"\nFound %zu Echo GPPC voice(s).\n", voices.size());
    if (voices.empty()) {
        wprintf(L"\nNo Echo GPPC voices are registered. Register the engine "
                L"(elevated) with:\n    regsvr32 EchoGPPCSAPI.dll\n");
        CoUninitialize();
        return 1;
    }

    if (list_only) {
        for (auto& v : voices) {
            v.token->Release();
        }
        CoUninitialize();
        return g_failures ? 1 : 0;
    }

    if (stress > 0) {
        run_stress(voices, stress);
        for (auto& v : voices) {
            v.token->Release();
        }
        wprintf(L"\n%ls\n", g_failures ? L"SELF-TEST FAILED" : L"Self-test passed.");
        CoUninitialize();
        return g_failures ? 1 : 0;
    }

    for (auto& entry : voices) {
        wprintf(L"\n=== %ls (ROM stem '%ls') ===\n", entry.name.c_str(), entry.stem.c_str());

        ISpVoice* voice = nullptr;
        hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              reinterpret_cast<void**>(&voice));
        if (FAILED(hr)) {
            fail(L"CoCreateInstance(SpVoice)", hr);
            entry.token->Release();
            continue;
        }

        hr = voice->SetVoice(entry.token);
        if (FAILED(hr)) {
            fail(L"ISpVoice::SetVoice", hr);
            voice->Release();
            entry.token->Release();
            continue;
        }
        voice->SetRate(rate);
        voice->SetVolume(static_cast<USHORT>(volume));

        std::wstring to_say = text;
        DWORD flags = SPF_DEFAULT;
        if (bookmarks) {
            // SAPI queues nothing at all until a notification sink exists,
            // whatever the interest mask says.
            hr = voice->SetNotifyWin32Event();
            if (FAILED(hr)) {
                fail(L"ISpVoice::SetNotifyWin32Event", hr);
            }
            voice->SetInterest(SPFEI(SPEI_TTS_BOOKMARK) | SPFEI(SPEI_WORD_BOUNDARY),
                               SPFEI(SPEI_TTS_BOOKMARK) | SPFEI(SPEI_WORD_BOUNDARY));
            to_say = L"<bookmark mark=\"11\"/>First part. "
                     L"<bookmark mark=\"22\"/>Second part.";
            flags |= SPF_IS_XML;
        }

        ISpStream* stream = nullptr;
        std::wstring wav_path;
        if (!wav_dir.empty()) {
            wav_path = wav_dir + L"\\sapi_" + entry.stem + L".wav";
            stream = make_wav_stream(wav_path, voice);
            if (stream) {
                hr = voice->SetOutput(stream, TRUE);
                if (FAILED(hr)) {
                    fail(L"ISpVoice::SetOutput", hr);
                }
            }
        } else if (!to_speaker) {
            wprintf(L"  (no --wav or --speak given; nothing to render)\n");
            voice->Release();
            entry.token->Release();
            continue;
        }

        const ULONGLONG started = GetTickCount64();
        hr = voice->Speak(to_say.c_str(), flags, nullptr);
        if (FAILED(hr)) {
            fail(L"ISpVoice::Speak", hr);
        }
        hr = voice->WaitUntilDone(60000);
        if (FAILED(hr)) {
            fail(L"ISpVoice::WaitUntilDone", hr);
        }
        const ULONGLONG elapsed = GetTickCount64() - started;

        if (bookmarks) {
            // Reading the events back off the voice confirms the engine really
            // placed them, and where in the audio stream it put them.
            SPEVENT ev{};
            ULONG fetched = 0;
            int seen = 0;
            while (voice->GetEvents(1, &ev, &fetched) == S_OK && fetched == 1) {
                if (ev.eEventId == SPEI_TTS_BOOKMARK) {
                    wprintf(L"  bookmark %llu at stream offset %llu bytes\n",
                            static_cast<unsigned long long>(ev.wParam),
                            ev.ullAudioStreamOffset);
                    ++seen;
                } else if (ev.eEventId == SPEI_WORD_BOUNDARY) {
                    wprintf(L"  word boundary: source offset %lu, length %llu, "
                            L"stream offset %llu bytes\n",
                            static_cast<unsigned long>(ev.lParam),
                            static_cast<unsigned long long>(ev.wParam),
                            ev.ullAudioStreamOffset);
                    ++seen;
                }
                clear_event(&ev);
                fetched = 0;
            }
            if (seen == 0) {
                if (stream) {
                    // Not a failure: SAPI advances its event queue from the
                    // audio device's playback position, and a file stream has
                    // none. The engine's own log shows the AddEvents calls.
                    wprintf(L"  (no events: output went to a file, so SAPI has no "
                            L"playback clock to fire them against -- "
                            L"rerun with --speak)\n");
                } else {
                    wprintf(L"  FAIL: no bookmark or word-boundary events "
                            L"were delivered\n");
                    ++g_failures;
                }
            }
        }

        if (stream) {
            voice->SetOutput(nullptr, FALSE);
            stream->Close();
            stream->Release();
            const long long size = file_size(wav_path);
            if (size < 1024) {
                wprintf(L"  FAIL: %ls is %lld bytes -- no audio was produced\n",
                        wav_path.c_str(), size);
                ++g_failures;
            } else {
                wprintf(L"  wrote %ls (%lld bytes) in %llu ms\n",
                        wav_path.c_str(), size, elapsed);
            }
        } else {
            wprintf(L"  spoke to the sound card in %llu ms\n", elapsed);
        }

        voice->Release();
        entry.token->Release();
    }

    wprintf(L"\n%ls\n", g_failures ? L"SELF-TEST FAILED" : L"Self-test passed.");
    CoUninitialize();
    return g_failures ? 1 : 0;
}
