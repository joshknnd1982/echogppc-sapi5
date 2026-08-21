#include <new>
#include <mutex>
#include <sapi.h>

#include "com.hpp"
#include "registry.hpp"
#include "echo_core.h"
#include "echo_log.h"
#include "echo_settings.h"
#include "ISpTTSEngineImpl.hpp"
#include "IEnumSpObjectTokensImpl.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
EchoGPPC::com::class_object_factory g_cls_obj_factory;
std::once_flag g_init_once;

const std::wstring token_enums_path = L"Software\\Microsoft\\Speech\\Voices\\TokenEnums";
const std::wstring enumerator_name  = L"EchoGPPC";

#if defined(_WIN64)
constexpr wchar_t LOG_TAG[] = L"sapi64";
#else
constexpr wchar_t LOG_TAG[] = L"sapi32";
#endif

// Deliberately not done from DllMain. Opening files, creating directories and
// allocating all run under the loader lock there, which is how an engine DLL
// turns a host's start-up into a deadlock.
void ensure_initialized()
{
    std::call_once(g_init_once, [] {
        EchoGPPC::log::init(LOG_TAG);
        EchoGPPC::sapi::settings_watcher().refresh();
        ECHO_INFO("Echo GPPC SAPI5 engine attaching (%d-bit)",
                  static_cast<int>(sizeof(void*) * 8));
        try {
            g_cls_obj_factory.register_class<EchoGPPC::sapi::IEnumSpObjectTokensImpl>();
            g_cls_obj_factory.register_class<EchoGPPC::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            ECHO_ERR("Failed to register class factories");
        }
    });
}

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

// SAPI discovers voices either from registry keys or from a token enumerator
// object. Registering an enumerator is what lets the voice list be built from
// the ROM files actually present, so adding or removing a Textalker image
// changes the available voices with no registry work at all.
void register_token_enumerator(HKEY root)
{
    using namespace EchoGPPC::sapi;
    using namespace EchoGPPC::registry;

    key enums_key(root, token_enums_path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
    key enum_key(enums_key, enumerator_name, KEY_SET_VALUE, true);

    enum_key.set(L"Echo GPPC Voices");
    enum_key.set(L"CLSID", clsid_to_string(__uuidof(IEnumSpObjectTokensImpl)));
}

void unregister_token_enumerator(HKEY root) noexcept
{
    using namespace EchoGPPC::registry;

    try {
        key enums_key(root, token_enums_path, KEY_ALL_ACCESS);
        enums_key.delete_subkey(enumerator_name);
    }
    catch (...) {
        // Already gone, or the key was never there. Either way there is
        // nothing an uninstall can usefully do about it.
    }
}

// One attempt at registering under `root`, reporting rather than throwing so
// the caller can fall back.
[[nodiscard]] bool try_register(HKEY root) noexcept
{
    try {
        EchoGPPC::com::class_registrar r(g_dll_handle, root);
        r.register_class<EchoGPPC::sapi::IEnumSpObjectTokensImpl>();
        r.register_class<EchoGPPC::sapi::ISpTTSEngineImpl>();
        register_token_enumerator(root);
        return true;
    }
    catch (...) {
        return false;
    }
}
}

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    ensure_initialized();
    return g_cls_obj_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return EchoGPPC::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    ensure_initialized();
    ECHO_INFO("DllRegisterServer");

    // Machine-wide only, and deliberately so. SAPI reads voice token
    // enumerators exclusively from HKLM: measured on Windows 11, an identical
    // registration under HKCU leaves the voices invisible to every SAPI
    // application even though COM resolves the CLSID from there quite happily.
    // A per-user fallback would therefore report success and deliver nothing,
    // so this fails honestly instead and says what to do about it.
    if (try_register(HKEY_LOCAL_MACHINE)) {
        ECHO_INFO("DllRegisterServer: registered for all users (HKLM)");
        return S_OK;
    }

    // Clean up anything a previous per-user attempt left behind, so a stale
    // HKCU CLSID cannot shadow a later machine-wide install.
    unregister_token_enumerator(HKEY_CURRENT_USER);
    try {
        EchoGPPC::com::class_registrar r(g_dll_handle, HKEY_CURRENT_USER);
        r.unregister_class<EchoGPPC::sapi::IEnumSpObjectTokensImpl>();
        r.unregister_class<EchoGPPC::sapi::ISpTTSEngineImpl>();
    }
    catch (...) {
    }

    ECHO_ERR("DllRegisterServer: could not write to HKLM. SAPI only reads voice "
             "enumerators from HKLM, so this must run elevated.");
    return E_ACCESSDENIED;
}

STDAPI DllUnregisterServer()
{
    ensure_initialized();
    ECHO_INFO("DllUnregisterServer");

    // Both hives are cleaned, whichever this build registered into: leaving a
    // stale CLSID behind would point SAPI at a DLL that is no longer there.
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        unregister_token_enumerator(root);
        try {
            EchoGPPC::com::class_registrar r(g_dll_handle, root);
            r.unregister_class<EchoGPPC::sapi::IEnumSpObjectTokensImpl>();
            r.unregister_class<EchoGPPC::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            // Missing keys are the normal case for whichever hive was not used.
        }
    }
    ECHO_INFO("DllUnregisterServer: done");
    return S_OK;
}
