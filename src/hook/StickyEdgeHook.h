#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "windowtracking/StickyEdges.h"

namespace polish {

// The input side of sticky edges (windowtracking/StickyEdges.h has the
// geometry): a WH_MOUSE_LL hook of its own that holds the pointer at
//
//   - the edges of the active app ("app sticky"), and
//   - the edges of the monitor's work area and of the monitor itself, which is
//     the taskbar's edge and the seam to the next monitor ("monitor sticky"),
//
// until it has been pushed on past the edge by the resistance distance.
//
// How it holds: a move that would cross is swallowed and the cursor is put at
// the edge with SetCursorPos instead. Swallowing a move freezes the pointer
// (TaskbarHook.h measured it, as something to avoid there); here that is the
// point, and SetCursorPos lets the part of the move along the edge through.
//
// Same rules as the other low-level hooks: everything the callback needs is a
// plain copy set from the installing thread (which is also the thread the
// callback runs on, so no lock), the callback queries nothing but the cursor,
// and it returns promptly or Windows silently unhooks it -- hence
// EnsureInstalled for the watchdog timer.
//
// App sticky stands down while any mouse button is held, so dragging something
// to another app, selecting text past the edge and moving or resizing a window
// all stay free. Monitor sticky does not: holding a dragged window at the seam
// between monitors is what lets it snap to that side.
//
// Resize handles ("sticky resize handles"): inside the active app, when the
// pointer shows a resize arrow -- a splitter, a window border -- it is held
// there for up to a second, or until pushed off, so the handle is easy to
// grab. Read with GetCursorInfo, which is a plain read of the current cursor.
// Only the system's own arrows are recognized; an app drawing a custom one is
// not held.
class StickyEdgeHook {
public:
    struct Monitor {
        RECT monitor{};
        RECT work{};
        UINT dpi = USER_DEFAULT_SCREEN_DPI;
    };

    // Events reported to `messageWindow` for logging, as kEventMessage:
    // wParam is the event, lParam the kind of edge.
    enum class Event : WPARAM { Held = 1, BrokeThrough = 2 };
    enum class EdgeKind : LPARAM { App = 0, WorkArea = 1, Monitor = 2, ResizeHandle = 3 };
    static constexpr UINT kEventMessage = WM_APP + 42;

    explicit StickyEdgeHook(HWND messageWindow);
    ~StickyEdgeHook();

    StickyEdgeHook(const StickyEdgeHook&) = delete;
    StickyEdgeHook& operator=(const StickyEdgeHook&) = delete;

    void SetMonitors(std::vector<Monitor> monitors) { monitors_ = std::move(monitors); }
    // The active app's outer window rect, or nullopt when it has none worth
    // holding the pointer in (desktop, taskbar, maximized, Polish's own).
    void SetActiveApp(std::optional<RECT> rect) { activeApp_ = rect; }
    // The active app's window rect even when maximized or full screen: where
    // resize handles hold the pointer. Nullopt for no app (desktop, taskbar,
    // Polish's own).
    void SetActiveWindow(std::optional<RECT> rect) { activeWindow_ = rect; }
    void SetEnabled(bool app, bool monitor, bool resize) {
        appEnabled_ = app;
        monitorEnabled_ = monitor;
        resizeEnabled_ = resize;
    }
    void SetResistanceDip(UINT dip) { resistanceDip_ = dip; }

    bool IsInstalled() const { return hook_ != nullptr; }
    bool EnsureInstalled();

private:
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
    // True to swallow the move.
    bool HandleMove(POINT to);
    bool HandleResizeHold(POINT from, POINT to, int threshold, UINT dpi);
    ResizeAxis CurrentResizeAxis() const;
    bool Install();
    void Uninstall();

    HWND messageWindow_;
    HHOOK hook_ = nullptr;
    ULONGLONG lastEventTick_ = 0;

    std::vector<Monitor> monitors_;
    std::optional<RECT> activeApp_;
    std::optional<RECT> activeWindow_;
    bool appEnabled_ = true;
    bool monitorEnabled_ = true;
    bool resizeEnabled_ = true;
    UINT resistanceDip_ = 120;

    // The system's resize arrows. Shared cursors, so the handle an app shows
    // is this same handle whichever process loaded it.
    HCURSOR sizeWe_ = nullptr;
    HCURSOR sizeNs_ = nullptr;
    HCURSOR sizeNwse_ = nullptr;
    HCURSOR sizeNesw_ = nullptr;

    StickyState state_;
    ResizeHoldState resizeState_;
};

}  // namespace polish
