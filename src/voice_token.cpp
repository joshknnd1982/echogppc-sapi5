#include <new>
#include <comdef.h>
#include "voice_token.hpp"
#include "ISpTTSEngineImpl.hpp"

namespace EchoGPPC {
namespace sapi {

voice_token::voice_token(const VoiceRom& rom)
{
    set(rom.display);

    utils::out_ptr<wchar_t> clsid_str(CoTaskMemFree);
    StringFromCLSID(__uuidof(ISpTTSEngineImpl), clsid_str.address());
    set(L"CLSID", clsid_str.get());

    // The ROM stem is carried on the token so the engine can map the voice
    // SAPI chose back to a pair of files on disk without re-deriving it from
    // the display name, which the user may one day see localised.
    set(L"EchoRomStem", rom.stem);

    attributes_[L"Name"]     = rom.display;
    attributes_[L"Vendor"]   = L"Street Electronics (emulated)";
    attributes_[L"Age"]      = L"Adult";
    attributes_[L"Gender"]   = L"Male";
    // 409 is en-US. The Echo speaks English and nothing else: Textalker's
    // letter-to-sound rules are English and there is no other ROM.
    attributes_[L"Language"] = L"409";
    attributes_[L"Version"]  = rom.banner;
    // Hosts show this in a voice picker; naming the hardware is more use to
    // someone choosing a voice than repeating the vendor.
    attributes_[L"Description"] =
        L"Echo GPPC (Street Electronics Echo II) running Textalker " + rom.banner;
}

STDMETHODIMP voice_token::OpenKey(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey)
{
    if (!pszSubKeyName) {
        return E_INVALIDARG;
    }
    if (!ppSubKey) {
        return E_POINTER;
    }
    *ppSubKey = nullptr;

    try {
        if (!str_equal(pszSubKeyName, L"Attributes")) {
            return SPERR_NOT_FOUND;
        }

        com::object<ISpDataKeyImpl> obj;
        for (const auto& [key, value] : attributes_) {
            obj->set(key, value);
        }

        com::interface_ptr<ISpDataKey> int_ptr(obj);
        *ppSubKey = int_ptr.get();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP voice_token::EnumKeys(ULONG Index, LPWSTR* ppszSubKeyName)
{
    if (!ppszSubKeyName) {
        return E_POINTER;
    }
    *ppszSubKeyName = nullptr;

    if (Index > 0) {
        return SPERR_NO_MORE_ITEMS;
    }

    try {
        *ppszSubKeyName = com::strdup(L"Attributes");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}
}
}
