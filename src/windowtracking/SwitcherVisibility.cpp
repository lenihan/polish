#include "windowtracking/SwitcherVisibility.h"

#include <inspectable.h>
#include <objbase.h>
#include <shobjidl.h>

#include <format>

#include "util/AppIdentity.h"
#include "util/Logging.h"

namespace polish {

namespace {

// {C2F03A33-21F5-47FA-B4BB-156362A2F239} -- the ImmersiveShell service
// host, which vends the shell's internal app-view interfaces.
const CLSID kClsidImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};

// {1841C6D7-4F9D-42C0-AF41-8747538F10E5} -- IApplicationViewCollection.
// Stable across Windows 10 and 11 so far, unlike IApplicationView's own
// layout (see the header).
const IID kIidApplicationViewCollection = {
    0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};

// The subset of IApplicationView this code actually uses, declared far
// enough down the vtable to reach SetShowInSwitchers. Every method above
// it must be declared with the right signature purely so the slots line
// up -- none of them are called except GetAppUserModelId, which is the
// layout check (see the header). Derived from IInspectable, which
// supplies the first six slots.
struct IApplicationViewLayout : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchTo() = 0;
    virtual HRESULT STDMETHODCALLTYPE TryInvokeBack(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetThumbnailWindow(HWND*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitor(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVisibility(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCloak(int, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPosition(REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPosition(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAfterWindow(HWND) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetExtendedFramePosition(RECT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppUserModelId(PWSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAppUserModelId(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsEqualByAppUserModelId(PCWSTR, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewState(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetViewState(UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNeediness(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastActivationTimestamp(ULONGLONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLastActivationTimestamp(ULONGLONG) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVirtualDesktopId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVirtualDesktopId(REFGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShowInSwitchers(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShowInSwitchers(int) = 0;
};

struct IApplicationViewCollectionLayout : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, IApplicationViewLayout**) = 0;
};

// Set once a call through the undocumented vtable faults or the layout
// check fails -- there's no reason to keep trying for the rest of the
// session once this build of Windows has been shown not to match.
bool g_switcherApiUnusable = false;

// SEH-guarded, and deliberately free of anything needing C++ unwinding
// so __try/__except is legal here. A fault means the vtable layout on
// this build of Windows isn't the one declared above.
HRESULT GuardedGetAppUserModelId(IApplicationViewLayout* view, PWSTR* out) {
    __try {
        return view->GetAppUserModelId(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

HRESULT GuardedSetShowInSwitchers(IApplicationViewLayout* view, int shown) {
    __try {
        return view->SetShowInSwitchers(shown);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

// Confirms the vtable really is laid out the way this file declares, by
// calling an earlier slot whose answer is independently known. See the
// header's guard 1.
bool VtableLayoutMatches(IApplicationViewLayout* view, const std::wstring& expectedAumid) {
    PWSTR reported = nullptr;
    if (FAILED(GuardedGetAppUserModelId(view, &reported)) || reported == nullptr) {
        LogDebug(L"[Polish] Switchers: IApplicationView layout check failed (GetAppUserModelId did not return) -- "
                 L"leaving this window's taskbar/Alt+Tab entry alone");
        return false;
    }
    const bool matches = expectedAumid == reported;
    if (!matches) {
        LogDebug(std::format(L"[Polish] Switchers: IApplicationView layout check MISMATCH -- slot returned \"{}\", "
                              L"expected \"{}\". This build of Windows lays the interface out differently; leaving "
                              L"the window alone.",
                              reported, expectedAumid));
    }
    CoTaskMemFree(reported);
    return matches;
}

}  // namespace

bool SetWindowShownInSwitchers(HWND hwnd, bool shown) {
    if (g_switcherApiUnusable) {
        return false;
    }
    // The layout check needs an AUMID resolved independently of this
    // interface; without one there's nothing to verify against, so don't
    // risk the call at all.
    const std::optional<std::wstring> expectedAumid = GetPackagedAppAumid(hwnd);
    if (!expectedAumid.has_value()) {
        return false;  // not a packaged app -- the documented levers cover these
    }

    IServiceProvider* shell = nullptr;
    if (FAILED(CoCreateInstance(kClsidImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
                                 reinterpret_cast<void**>(&shell))) ||
        shell == nullptr) {
        LogDebug(L"[Polish] Switchers: ImmersiveShell service unavailable");
        g_switcherApiUnusable = true;
        return false;
    }

    IApplicationViewCollectionLayout* views = nullptr;
    const HRESULT queryResult = shell->QueryService(kIidApplicationViewCollection, kIidApplicationViewCollection,
                                                     reinterpret_cast<void**>(&views));
    shell->Release();
    if (FAILED(queryResult) || views == nullptr) {
        LogDebug(std::format(L"[Polish] Switchers: IApplicationViewCollection unavailable, hr=0x{:08x}",
                              static_cast<unsigned long>(queryResult)));
        g_switcherApiUnusable = true;
        return false;
    }

    IApplicationViewLayout* view = nullptr;
    const HRESULT viewResult = views->GetViewForHwnd(hwnd, &view);
    views->Release();
    if (FAILED(viewResult) || view == nullptr) {
        LogDebug(std::format(L"[Polish] Switchers: no app view for hwnd={}, hr=0x{:08x}",
                              reinterpret_cast<void*>(hwnd), static_cast<unsigned long>(viewResult)));
        return false;
    }

    bool applied = false;
    if (VtableLayoutMatches(view, *expectedAumid)) {
        const HRESULT setResult = GuardedSetShowInSwitchers(view, shown ? 1 : 0);
        applied = SUCCEEDED(setResult);
        LogDebug(std::format(L"[Polish] Switchers: hwnd={} show={} -- {} (hr=0x{:08x})",
                              reinterpret_cast<void*>(hwnd), shown, applied ? L"applied" : L"FAILED",
                              static_cast<unsigned long>(setResult)));
    } else {
        // The layout is not what this was written against; don't keep
        // poking at it for the rest of the session.
        g_switcherApiUnusable = true;
    }
    view->Release();
    return applied;
}

}  // namespace polish
