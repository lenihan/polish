#include "util/AppResolver.h"

// objbase.h before anything that declares COM interfaces, same reason
// UiaWorker.cpp documents: the project builds with WIN32_LEAN_AND_MEAN, so
// windows.h alone leaves the `interface` macro undefined.
#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
// shellapi.h, not shlobj.h: SHGetPropertyStoreForWindow is declared there
// and nowhere else in the SDK.
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <format>

#include "util/Logging.h"

using Microsoft::WRL::ComPtr;

namespace polish {
namespace {

// IApplicationResolver, declared here because it appears in no SDK header.
// CLSID/IID and the vtable order are the Windows 8+ shape, unchanged
// through Windows 11 -- verified live against build 26200 before this was
// written (see AppResolver.h). Only the first four methods are declared:
// the interface continues past them, but a COM vtable is positional, so
// anything we never call simply need not be named. Declaring fewer methods
// than the real interface has is safe; declaring them in the wrong order
// would not be.
MIDL_INTERFACE("de25675a-72de-44b4-9373-05170450c140")
IApplicationResolver : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcut(IShellItem* item, LPWSTR* appId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcutObject(IShellLink* link, IShellItem* item,
                                                                LPWSTR* appId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForWindow(HWND hwnd, LPWSTR* appId, BOOL* pinningPrevented,
                                                        BOOL* explicitAppId, BOOL* embeddedShortcutValid) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForProcess(DWORD processId, LPWSTR* appId, BOOL* pinningPrevented,
                                                         BOOL* explicitAppId, BOOL* embeddedShortcutValid) = 0;
};

// CLSID_StartMenuCacheAndAppResolver.
const CLSID kAppResolverClsid = {
    0x660b90c8, 0x73a9, 0x4b58, {0x8c, 0xae, 0x35, 0x5b, 0x7f, 0x55, 0x34, 0x1b}};

// The explicit AUMID a window set on itself, if any. The cheap path -- one
// in-process property-store read, no app resolution -- but it answers for
// only a minority of windows, which is exactly why it is not the only path
// (see AppIdForWindow's comment).
std::optional<std::wstring> ExplicitWindowAppId(HWND hwnd) {
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) {
        return std::nullopt;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    if (FAILED(store->GetValue(PKEY_AppUserModel_ID, &value))) {
        PropVariantClear(&value);
        return std::nullopt;
    }
    std::optional<std::wstring> result;
    if (value.vt == VT_LPWSTR && value.pwszVal != nullptr && value.pwszVal[0] != L'\0') {
        result = std::wstring(value.pwszVal);
    }
    PropVariantClear(&value);
    return result;
}

}  // namespace

struct AppResolver::Impl {
    ComPtr<IApplicationResolver> resolver;
};

AppResolver::AppResolver() : impl_(std::make_unique<Impl>()) {
    const HRESULT hr = CoCreateInstance(kAppResolverClsid, nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(IApplicationResolver), &impl_->resolver);
    if (FAILED(hr) || !impl_->resolver) {
        impl_->resolver.Reset();
        // Not fatal and not chased: the caller disables its feature rather
        // than guessing at window identity. See docs/LIMITATIONS.md.
        LogDebug(std::format(L"[Polish] Taskbar: WARNING app resolver unavailable, hr=0x{:08x}",
                             static_cast<uint32_t>(hr)));
        return;
    }
    LogDebug(L"[Polish] Taskbar: app resolver ready");
}

AppResolver::~AppResolver() = default;

bool AppResolver::IsAvailable() const { return impl_ && impl_->resolver; }

std::optional<std::wstring> AppResolver::AppIdForWindow(HWND hwnd) const {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return std::nullopt;
    }
    if (std::optional<std::wstring> explicitId = ExplicitWindowAppId(hwnd)) {
        return explicitId;
    }
    if (!IsAvailable()) {
        return std::nullopt;
    }

    LPWSTR raw = nullptr;
    BOOL pinningPrevented = FALSE;
    BOOL explicitAppId = FALSE;
    BOOL embeddedShortcutValid = FALSE;
    const HRESULT hr = impl_->resolver->GetAppIDForWindow(hwnd, &raw, &pinningPrevented, &explicitAppId,
                                                          &embeddedShortcutValid);
    if (FAILED(hr) || raw == nullptr) {
        return std::nullopt;
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

std::wstring StripAppIdPrefix(std::wstring_view automationId) {
    constexpr std::wstring_view kPrefix = L"Appid: ";
    if (automationId.size() >= kPrefix.size() && automationId.substr(0, kPrefix.size()) == kPrefix) {
        return std::wstring(automationId.substr(kPrefix.size()));
    }
    return std::wstring(automationId);
}

}  // namespace polish
