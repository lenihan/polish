#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace polish {

// One row in the panel -- icon and title are captured at Show() time
// (borrowed HICON, same "never destroyed here" contract as
// GroupChromeWindow::SetMemberIcons) since the underlying window could
// close or its title could change while the row is still displayed.
struct AltTabListRow {
    HWND hwnd = nullptr;
    std::wstring title;
    HICON icon = nullptr;  // borrowed, may be nullptr (falls back to text-only)
};

// A translucent panel shown alongside every Alt+Tab session, listing
// every active candidate window (icon + title, one row each) with the
// currently Tab-highlighted row visually marked -- so "where am I in the
// cycle, and how many are there" is answered by looking at the list, not
// just by which single on-screen window happens to be undimmed. See
// PLAN.md's Alt+Tab-improvements plan (M3 lands the active-window list;
// M4 adds a minimized section below it; M5 adds per-row action buttons).
//
// Created once (e.g. at startup, alongside AltTabDimOverlay's pool and
// AltTabHighlightBorder) and only shown/hidden/repopulated per session,
// never destroyed/recreated -- Alt+Tab is exactly the latency-sensitive
// path that already caused a real first-show flash bug once, when a
// different piece of this feature's UI was created lazily on first use
// instead of pre-warmed. This deliberately does NOT follow
// GroupPickerWindow's teardown-per-show/nested-message-loop pattern.
//
// Rendering is plain, flat-opaque GDI (double-buffered WM_PAINT, the same
// technique GroupChromeWindow's tab strip uses) -- not the DIB+GDI+
// pipeline AltTabHighlightBorder uses, which exists specifically for a
// translucent gradient this flat row content doesn't need. The panel's
// own "frosted, see-through-but-legible" background is instead one
// constant SetLayeredWindowAttributes(LWA_ALPHA) value applied to the
// whole window, the same simple technique AltTabDimOverlay already uses
// for its own uniform-opacity dimming.
//
// This window is WS_EX_NOACTIVATE (like the dim overlays and highlight
// border) and never receives keyboard focus, so it does not itself
// handle Tab/arrow-key input -- AltTabHook recognizes those keys
// system-wide (see its class comment) and the owner (main.cpp) drives
// this class's Show/SetHighlight in response. It DOES need to handle
// mouse clicks directly (row activation), which is why it's deliberately
// not WS_EX_TRANSPARENT the way the dim overlays are -- AltTabHook's
// isOwnUI callback (see its SetIsOwnUI) is how the mouse hook knows not
// to treat a click landing here as a generic commit trigger.
class AltTabListWindow {
public:
    explicit AltTabListWindow(HINSTANCE instance);
    ~AltTabListWindow();

    AltTabListWindow(const AltTabListWindow&) = delete;
    AltTabListWindow& operator=(const AltTabListWindow&) = delete;

    // Full repopulate: replaces the row list, positions the panel
    // centered on the monitor containing `monitorAnchor`, marks
    // `highlightIndex` as highlighted, and shows it. Callers should
    // prefer SetHighlight (below) when the row content itself hasn't
    // changed since the last Show -- this rebuilds row layout from
    // scratch and is comparatively more work.
    void Show(const std::vector<AltTabListRow>& rows, size_t highlightIndex, HWND monitorAnchor);

    // Moves the highlighted-row marker and repaints, without touching row
    // content or repositioning the panel -- the cheap path for a cycle
    // where the row set itself didn't change. No-op if index is out of
    // range for the current row list.
    void SetHighlight(size_t index);

    void Hide();
    bool IsVisible() const;
    HWND WindowHandle() const { return window_; }

    // True if `screenPt` falls within this panel's current window rect --
    // what AltTabHook's isOwnUI callback (see SetIsOwnUI) checks before
    // treating a click as a generic session-commit trigger. False if the
    // panel isn't currently visible.
    bool ContainsPoint(POINT screenPt) const;

    // Fired when a row is activated: a left-click on it, landing inside
    // this window (see ContainsPoint) -- with that row's index into the
    // vector most recently passed to Show.
    void SetOnRowActivated(std::function<void(size_t index)> callback) { onRowActivated_ = std::move(callback); }

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& clientRect) const;
    std::vector<RECT> ComputeRowRects(UINT dpi) const;
    // Resizes/repositions the panel (content-sized from the current row
    // count) centered on the monitor containing `monitorAnchor`, at the
    // given DPI.
    void Reposition(HWND monitorAnchor, UINT dpi);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::vector<AltTabListRow> rows_;
    size_t highlightIndex_ = 0;
    std::function<void(size_t)> onRowActivated_;
};

}  // namespace polish
