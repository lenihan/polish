#include "hook/StickyEdgeHook.h"

#include <array>

namespace polish {

namespace {

StickyEdgeHook* g_instance = nullptr;

// As TaskbarHook's: no event for this long while the system has had input
// means Windows has dropped the hook.
constexpr ULONGLONG kPresumedDeadMs = 5000;

// Mouse messages Windows synthesizes from pen and touch carry this in
// dwExtraInfo (MI_WP_SIGNATURE). A pen points at an absolute position, so
// holding it back would only put the cursor somewhere the pen is not.
constexpr ULONG_PTR kPenTouchSignatureMask = 0xFFFFFF00;
constexpr ULONG_PTR kPenTouchSignature = 0xFF515700;

bool Inside(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

bool AnyButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
}

}  // namespace

StickyEdgeHook::StickyEdgeHook(HWND messageWindow) : messageWindow_(messageWindow) {
    g_instance = this;
    Install();
}

StickyEdgeHook::~StickyEdgeHook() {
    Uninstall();
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

bool StickyEdgeHook::Install() {
    if (hook_ == nullptr) {
        hook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        lastEventTick_ = GetTickCount64();
    }
    return hook_ != nullptr;
}

void StickyEdgeHook::Uninstall() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
}

bool StickyEdgeHook::EnsureInstalled() {
    if (hook_ == nullptr) {
        return Install();
    }
    LASTINPUTINFO lastInput{};
    lastInput.cbSize = sizeof(lastInput);
    if (!GetLastInputInfo(&lastInput)) {
        return false;
    }
    // See TaskbarHook::EnsureInstalled for the rollover-safe comparison.
    const auto elapsed = static_cast<LONG>(lastInput.dwTime - static_cast<DWORD>(lastEventTick_));
    if (elapsed < static_cast<LONG>(kPresumedDeadMs)) {
        return false;
    }
    Uninstall();
    return Install();
}

LRESULT CALLBACK StickyEdgeHook::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance != nullptr) {
        g_instance->lastEventTick_ = GetTickCount64();
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const bool injected = (data->flags & LLMHF_INJECTED) != 0;
        const bool penOrTouch = (data->dwExtraInfo & kPenTouchSignatureMask) == kPenTouchSignature;
        if (wParam == WM_MOUSEMOVE && !injected && !penOrTouch && g_instance->HandleMove(data->pt)) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool StickyEdgeHook::HandleMove(POINT to) {
    if (!appEnabled_ && !monitorEnabled_) {
        return false;
    }
    // The cursor has not moved yet when the hook sees the move, so this is
    // where it is coming from -- including wherever SetCursorPos last put it.
    POINT from{};
    if (!GetCursorPos(&from) || (from.x == to.x && from.y == to.y)) {
        return false;
    }
    const Monitor* monitor = nullptr;
    for (const Monitor& m : monitors_) {
        if (Inside(m.monitor, from)) {
            monitor = &m;
            break;
        }
    }

    std::array<Barrier, 3> barriers{};
    std::array<EdgeKind, 3> kinds{};
    size_t count = 0;
    if (appEnabled_ && activeApp_.has_value() && Inside(*activeApp_, from) && !AnyButtonDown()) {
        barriers[count] = Barrier{*activeApp_};
        kinds[count++] = EdgeKind::App;
    }
    if (monitorEnabled_ && monitor != nullptr) {
        barriers[count] = Barrier{monitor->work};
        kinds[count++] = EdgeKind::WorkArea;
        barriers[count] = Barrier{monitor->monitor};
        kinds[count++] = EdgeKind::Monitor;
    }
    if (count == 0) {
        state_ = StickyState{};
        return false;
    }

    const UINT dpi = monitor != nullptr ? monitor->dpi : USER_DEFAULT_SCREEN_DPI;
    const int threshold = MulDiv(static_cast<int>(resistanceDip_), static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int barrierBefore = state_.barrier;
    const bool wasPushing = state_.pushed > 0;
    const StickyResult result =
        StepSticky(std::span<const Barrier>(barriers.data(), count), from, to, threshold, state_, GetTickCount64());

    if (result.hold) {
        if (!wasPushing || state_.barrier != barrierBefore) {
            PostMessageW(messageWindow_, kEventMessage, static_cast<WPARAM>(Event::Held),
                         static_cast<LPARAM>(kinds[static_cast<size_t>(state_.barrier)]));
        }
        SetCursorPos(result.clamped.x, result.clamped.y);
        return true;
    }
    if (wasPushing && barrierBefore >= 0 && static_cast<size_t>(barrierBefore) < count && state_.barrier < 0 &&
        !Inside(barriers[static_cast<size_t>(barrierBefore)].rect, to)) {
        PostMessageW(messageWindow_, kEventMessage, static_cast<WPARAM>(Event::BrokeThrough),
                     static_cast<LPARAM>(kinds[static_cast<size_t>(barrierBefore)]));
    }
    return false;
}

}  // namespace polish
