#include "util/WindowIcon.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <format>
#include <iterator>
#include <optional>
#include <string>

#include "util/AppIdentity.h"
#include "util/Logging.h"

namespace polish {

namespace {

// Packaged/UWP-hosted apps (Settings, Store, ...) often run their real
// UI inside a frame window that doesn't answer WM_GETICON or carry a
// meaningful class icon -- but the shell always knows an icon for the
// process's own executable, the same one Explorer/the taskbar would
// show for it. Returns nullptr on any failure (e.g. no permission to
// query the process) rather than throwing -- this is a last-resort
// fallback, not a load-bearing path.
HICON GetProcessExecutableIcon(HWND hwnd) {
    const std::optional<std::wstring> path = GetWindowProcessImagePath(hwnd);
    if (!path.has_value()) {
        return nullptr;
    }
    SHFILEINFOW fileInfo{};
    if (SHGetFileInfoW(path->c_str(), 0, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_SMALLICON) == 0) {
        return nullptr;
    }
    // Caller-owned (unlike every other path in GetWindowIconHandle) --
    // deliberately leaked rather than destroyed here or pushed onto the
    // caller; see GetWindowIconHandle's own header comment for why.
    return fileInfo.hIcon;
}

// Converts a 32bpp premultiplied-BGRA bitmap (what
// IShellItemImageFactory hands back) into an HICON, which is what every
// caller of this file already expects. CreateIconIndirect wants a
// color+mask pair; a fully-opaque mask is right here because the color
// bitmap already carries its own alpha channel.
HICON IconFromBitmap(HBITMAP colorBitmap) {
    BITMAP info{};
    if (GetObjectW(colorBitmap, sizeof(info), &info) == 0) {
        return nullptr;
    }
    HBITMAP mask = CreateBitmap(info.bmWidth, info.bmHeight, 1, 1, nullptr);
    if (mask == nullptr) {
        return nullptr;
    }
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(mask);
    return icon;
}

// The real icon of a packaged (UWP/Store) app, resolved the way the
// shell itself does it: the app's identity is its Application User Model
// ID, and the shell exposes every installed app under the AppsFolder
// known folder keyed by exactly that.
//
// This exists because the two simpler routes both come back generic for
// a packaged app: its frame window belongs to the shared
// ApplicationFrameHost.exe (so the process's own executable icon is that
// host's, not the app's), and even after finding the app's real process
// via its CoreWindow child, a packaged app's executable typically has no
// embedded icon resource at all -- its icons live in the package
// manifest's assets, which SHGetFileInfoW doesn't read. Confirmed live:
// both routes produced the generic placeholder for Calculator.
//
// nullptr for anything that isn't a packaged app, which is the
// expected, silent case.
HICON GetPackagedAppIcon(const std::wstring& aumid) {
    IShellItem* item = nullptr;
    // KF_FLAG_DONT_VERIFY -- AppsFolder is virtual, there's no file to
    // stat, and verifying only slows the lookup down.
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, aumid.c_str(),
                                          IID_IShellItem, reinterpret_cast<void**>(&item))) ||
        item == nullptr) {
        LogDebug(std::format(L"[Polish] Icon: no AppsFolder item for AUMID \"{}\"", aumid));
        return nullptr;
    }

    IShellItemImageFactory* imageFactory = nullptr;
    HICON icon = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_IShellItemImageFactory, reinterpret_cast<void**>(&imageFactory))) &&
        imageFactory != nullptr) {
        HBITMAP bitmap = nullptr;
        // SIIGBF_ICONONLY so this never falls back to a *thumbnail* of
        // the app's content; BIGGERSIZEOK because the packaged logo is
        // usually only available at larger sizes and downscaling it is
        // better than getting nothing.
        const SIZE requested{16, 16};
        if (SUCCEEDED(imageFactory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) &&
            bitmap != nullptr) {
            icon = IconFromBitmap(bitmap);
            DeleteObject(bitmap);
        }
        imageFactory->Release();
    }
    item->Release();
    LogDebug(std::format(L"[Polish] Icon: AUMID \"{}\" -> packaged icon {}", aumid,
                          icon != nullptr ? L"resolved" : L"NOT resolved"));
    return icon;
}

// The packaged-app icon for the app that owns `hwnd`, if it is one.
HICON GetPackagedAppIconForWindow(HWND hwnd) {
    const std::optional<std::wstring> aumid = GetPackagedAppAumid(hwnd);
    return aumid.has_value() ? GetPackagedAppIcon(*aumid) : nullptr;
}

}  // namespace

HICON GetWindowIconHandle(HWND hwnd) {
    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    if (icon == nullptr) {
        // The real app's own process, if this is a UWP frame -- see this
        // function's own header comment for why ApplicationFrameHost.exe
        // (what a plain GetProcessExecutableIcon(hwnd) would resolve to
        // here) is the wrong process to ask. The packaged-app lookup
        // comes first because a packaged app's *executable* generally
        // carries no icon resource at all (see GetPackagedAppIcon).
        if (const HWND coreWindow = FindCoreWindowChild(hwnd); coreWindow != nullptr) {
            icon = GetPackagedAppIconForWindow(coreWindow);
            if (icon == nullptr) {
                icon = GetProcessExecutableIcon(coreWindow);
            }
        }
    }
    if (icon == nullptr) {
        icon = GetProcessExecutableIcon(hwnd);
    }
    return icon;
}

}  // namespace polish
