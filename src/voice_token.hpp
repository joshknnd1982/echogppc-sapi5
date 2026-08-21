#pragma once

#include <map>
#include <comdef.h>
#include <comip.h>

#include "utils.hpp"
#include "echo_core.h"
#include "ISpDataKeyImpl.hpp"

namespace EchoGPPC {
namespace sapi {

// An in-memory ISpDataKey standing in for the registry key SAPI would
// normally read a voice's attributes out of. Building these by hand is what
// lets the engine enumerate its voices from the ROM files on disk instead of
// requiring an installer to write one registry key per voice.
class voice_token : public ISpDataKeyImpl
{
public:
    explicit voice_token(const VoiceRom& rom);

    STDMETHOD(OpenKey)(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey) override;
    STDMETHOD(EnumKeys)(ULONG Index, LPWSTR* ppszSubKeyName) override;

private:
    [[nodiscard]] bool str_equal(const std::wstring& s1, const std::wstring& s2) const noexcept
    {
        return _wcsicmp(s1.c_str(), s2.c_str()) == 0;
    }

    using attribute_map = std::map<std::wstring, std::wstring, str_less>;

    attribute_map attributes_;
};
}
}
