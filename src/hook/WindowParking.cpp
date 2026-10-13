#include "hook/WindowParking.h"

#include "util/Logging.h"
#include "windowtracking/WindowFilters.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace polish {

namespace {

constexpr wchar_t kJournalKey[] = L"Software\\Polish\\Parked";

// What the journal stores per window. Plain data, fixed layout: it is read
// back by a later run of the same binary.
struct JournalEntry {
    uint32_t pid = 0;
    int64_t originalExStyle = 0;
    int32_t hadLayered = 0;
    int32_t alpha = 255;
    uint32_t colorKey = 0;
    uint32_t layeredFlags = 0;
    int32_t wasTopmost = 0;
    RECT originalRect{};
};

// The crash handler's view of what is parked: a fixed array, so the
// handler allocates nothing and takes no lock. Filled and cleared by
// ParkWindow/UnparkWindow, which run on the one UI thread.
constexpr size_t kMaxParked = 64;
struct LiveParked {
    HWND hwnd = nullptr;
    ParkRecord record;
};
LiveParked g_live[kMaxParked];

void AddLive(HWND hwnd, const ParkRecord& record) {
    for (LiveParked& slot : g_live) {
        if (slot.hwnd == nullptr || slot.hwnd == hwnd) {
            slot.hwnd = hwnd;
            slot.record = record;
            return;
        }
    }
    // Full: the journal still has it, which covers a hard kill.
}

void RemoveLive(HWND hwnd) {
    for (LiveParked& slot : g_live) {
        if (slot.hwnd == hwnd) {
            slot.hwnd = nullptr;
        }
    }
}

std::wstring KeyName(HWND hwnd) {
    return std::format(L"{:x}", reinterpret_cast<uintptr_t>(hwnd));
}

void WriteJournal(HWND hwnd, DWORD pid, const ParkRecord& record) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kJournalKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    JournalEntry entry;
    entry.pid = pid;
    entry.originalExStyle = record.originalExStyle;
    entry.hadLayered = record.hadLayered ? 1 : 0;
    entry.alpha = record.alpha;
    entry.colorKey = record.colorKey;
    entry.layeredFlags = record.layeredFlags;
    entry.wasTopmost = record.wasTopmost ? 1 : 0;
    entry.originalRect = record.originalRect;
    const std::wstring name = KeyName(hwnd);
    RegSetValueExW(key, name.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE*>(&entry), sizeof(entry));
    RegCloseKey(key);
}

void ClearJournal(HWND hwnd) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kJournalKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        const std::wstring name = KeyName(hwnd);
        RegDeleteValueW(key, name.c_str());
        RegCloseKey(key);
    }
}

// Puts the styles and rect back. Shared by the normal path, the crash
// filter and the journal replay -- allocation-free and exception-free,
// since the filter runs in a process that may be coming apart.
void ApplyRestore(HWND hwnd, const ParkRecord& record) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, record.originalExStyle);
    if (record.hadLayered) {
        SetLayeredWindowAttributes(hwnd, record.colorKey, record.alpha, record.layeredFlags);
    }
    // Always set explicitly: a ghost-topmost window must lose topmost, and
    // an originally-topmost one must keep it.
    SetWindowPos(hwnd, record.wasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

LONG WINAPI ParkingCrashFilter(EXCEPTION_POINTERS*) {
    for (LiveParked& slot : g_live) {
        if (slot.hwnd != nullptr) {
            ApplyRestore(slot.hwnd, slot.record);
            slot.hwnd = nullptr;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void ApplyParkStyles(HWND hwnd, LONG_PTR originalExStyle, ParkStrategy strategy) {
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, originalExStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
    if (strategy == ParkStrategy::GhostTopmost) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

}  // namespace

std::optional<ParkRecord> ParkWindow(HWND hwnd, ParkStrategy strategy) {
    if (hwnd == nullptr || !IsWindow(hwnd) || IsElevatedWindow(hwnd)) {
        return std::nullopt;
    }
    ParkRecord record;
    record.originalExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    record.wasTopmost = (record.originalExStyle & WS_EX_TOPMOST) != 0;
    GetWindowRect(hwnd, &record.originalRect);
    if (record.originalExStyle & WS_EX_LAYERED) {
        BYTE alpha = 255;
        COLORREF key = 0;
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) {
            // Per-pixel layered (UpdateLayeredWindow): the app owns its
            // alpha, and overwriting it would make it paint nothing.
            LogDebug(L"[Polish] Scale: refused, window is per-pixel layered");
            return std::nullopt;
        }
        record.hadLayered = true;
        record.alpha = alpha;
        record.colorKey = key;
        record.layeredFlags = flags;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Before the change, so a crash between the two leaves a journal
    // entry for a window that is merely un-parked, never the reverse.
    WriteJournal(hwnd, pid, record);
    AddLive(hwnd, record);
    ApplyParkStyles(hwnd, record.originalExStyle, strategy);
    return record;
}

void UnparkWindow(HWND hwnd, const ParkRecord& record) {
    ApplyRestore(hwnd, record);
    RemoveLive(hwnd);
    ClearJournal(hwnd);
}

bool IsStillParked(HWND hwnd, ParkStrategy strategy) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != (WS_EX_LAYERED | WS_EX_TRANSPARENT)) {
        return false;
    }
    BYTE alpha = 255;
    if (!GetLayeredWindowAttributes(hwnd, nullptr, &alpha, nullptr) || alpha != 1) {
        return false;
    }
    if (strategy == ParkStrategy::GhostTopmost && (ex & WS_EX_TOPMOST) == 0) {
        return false;
    }
    return true;
}

void ReapplyPark(HWND hwnd, ParkStrategy strategy) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    ApplyParkStyles(hwnd, GetWindowLongPtrW(hwnd, GWL_EXSTYLE), strategy);
}

void RestoreParkedFromJournal() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kJournalKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    // Names first, then act: deleting values while enumerating them
    // would skip entries.
    std::vector<std::wstring> names;
    for (DWORD index = 0;; ++index) {
        wchar_t name[64];
        DWORD nameLength = 64;
        if (RegEnumValueW(key, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        names.emplace_back(name, nameLength);
    }
    for (const std::wstring& name : names) {
        JournalEntry entry;
        DWORD size = sizeof(entry);
        DWORD type = 0;
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&entry), &size) ==
                ERROR_SUCCESS &&
            type == REG_BINARY && size == sizeof(entry)) {
            const auto hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::wcstoull(name.c_str(), nullptr, 16)));
            DWORD pid = 0;
            if (IsWindow(hwnd)) {
                GetWindowThreadProcessId(hwnd, &pid);
            }
            if (pid != 0 && pid == entry.pid) {
                ParkRecord record;
                record.originalExStyle = static_cast<LONG_PTR>(entry.originalExStyle);
                record.hadLayered = entry.hadLayered != 0;
                record.alpha = static_cast<BYTE>(entry.alpha);
                record.colorKey = entry.colorKey;
                record.layeredFlags = entry.layeredFlags;
                record.wasTopmost = entry.wasTopmost != 0;
                record.originalRect = entry.originalRect;
                ApplyRestore(hwnd, record);
                LogDebug(std::format(L"[Polish] Scale: restored window {} left parked by a previous run", name));
            }
        }
        RegDeleteValueW(key, name.c_str());
    }
    RegCloseKey(key);
}

void InstallParkingCrashHandler() {
    static bool installed = false;
    if (!installed) {
        SetUnhandledExceptionFilter(ParkingCrashFilter);
        installed = true;
    }
}

}  // namespace polish
