#pragma once

#include <atomic>
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "com.hpp"
#include "echo_core.h"
#include "echo_settings.h"

namespace EchoGPPC {
namespace sapi {

class __declspec(uuid("{e6b44e93-25dc-4907-bdf0-666f80a0d501}")) ISpTTSEngineImpl :
    public ISpTTSEngine, public ISpObjectWithToken
{
public:
    ISpTTSEngineImpl();
    ~ISpTTSEngineImpl();

    ISpTTSEngineImpl(const ISpTTSEngineImpl&) = delete;
    ISpTTSEngineImpl& operator=(const ISpTTSEngineImpl&) = delete;

    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId, WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    // Current settings for this engine's voice, re-read from disk if the
    // configuration utility has touched the file since the last utterance.
    [[nodiscard]] VoiceSettings current_settings();

    ISpObjectTokenPtr token_;
    VoiceRom          rom_;
    bool              rom_valid_ = false;

    // The format SAPI was told to expect. Audio MUST come out at this rate
    // for the rest of the stream even if the user changes the setting, so it
    // is captured at negotiation time rather than read per utterance.
    std::atomic<unsigned> negotiated_rate_{22050};
};

// Shared by every engine object in the process; watches the INI file.
[[nodiscard]] SettingsWatcher& settings_watcher();

}
}
