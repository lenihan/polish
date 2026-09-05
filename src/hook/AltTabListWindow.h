#pragma once

#include <windows.h>

#include <functional>
#include <optional>
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
    // True for a row in the minimized section (see PLAN.md's Alt+Tab-
    // improvements M4) -- drawn in a muted color, below a divider from
    // the active-window rows above it. The caller (main.cpp) is
    // responsible for keeping all minimized rows contiguous at the end
    // of the vector passed to Show(); this class just draws a divider
    // before the first row where this differs from the previous one.
    bool minimized = false;
};

// A translucent panel shown alongside every Alt+Tab session, listing
// active candidate windows (icon + title, one row each) with the
// currently Tab-highlighted row visually marked -- so "where am I in the
// cycle, and how many are there" is answered by looking at the list, not
// just by which single on-screen window happens to be undimmed. A second
// section below it (rows with AltTabListRow::minimized set, separated by
// a divider) lists minimized windows -- reachable via Up/Down but never
// part of the Tab/Shift+Tab cycle itself (see PLAN.md's Alt+Tab-
// improvements M4; M5 -- per-row action buttons -- is still unbuilt).
//
// On a multi-monitor setup, per explicit user request, one instance of
// this class exists *per connected monitor* (see main.cpp's
// g_altTabPanels) rather than a single panel following the cursor/
// foreground window around: every monitor shows its own subset of the
// full candidate list (which windows land in a given monitor's subset is
// decided by the caller, not this class), and only the one monitor whose
// subset actually contains the globally-highlighted window shows a
// highlighted row -- the rest show `std::nullopt` (no highlight). The
// underlying candidate order groups the current monitor's windows first,
// then each other monitor's in turn (see GetMonitorsCurrentFirst in
// main.cpp), so a plain Tab/Shift+Tab walking that single flat list
// naturally finishes the current monitor before continuing onto the
// next one -- this class has no monitor-crossing logic of its own to
// worry about, it only ever renders whatever subset+highlight it's told.
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
    // centered on `targetMonitor`, marks `highlightIndex` as highlighted
    // (nullopt if none of this panel's own rows are the globally-
    // highlighted window), and shows it. Callers should prefer
    // SetHighlight (below) when the row content itself hasn't changed
    // since the last Show -- this rebuilds row layout from scratch and
    // is comparatively more work. A caller with zero rows for this
    // monitor this cycle should call Hide() instead of Show with an
    // empty vector.
    void Show(const std::vector<AltTabListRow>& rows, std::optional<size_t> highlightIndex, HMONITOR targetMonitor);

    // Moves the highlighted-row marker (or clears it, if nullopt -- this
    // monitor's subset no longer contains the globally-highlighted
    // window) and repaints, without touching row content or
    // repositioning the panel -- the cheap path for a cycle where the
    // row set itself didn't change.
    void SetHighlight(std::optional<size_t> index);

    void Hide();
    bool IsVisible() const;
    HWND WindowHandle() const { return window_; }

    // True if `screenPt` falls within this panel's current window rect --
    // what AltTabHook's isOwnUI callback (see SetIsOwnUI) checks before
    // treating a click as a generic session-commit trigger. False if the
    // panel isn't currently visible.
    bool ContainsPoint(POINT screenPt) const;

    // Fired when a row is activated: a left-click on it, landing inside
    // this window (see ContainsPoint) -- with that row's own HWND
    // (rather than a local index, which would be meaningless to the
    // caller without knowing which monitor's panel/subset it came from).
    void SetOnRowActivated(std::function<void(HWND)> callback) { onRowActivated_ = std::move(callback); }

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& clientRect) const;
    std::vector<RECT> ComputeRowRects(UINT dpi) const;
    // Resizes/repositions the panel (content-sized from the current row
    // count) centered on `targetMonitor`, at the given DPI.
    void Reposition(HMONITOR targetMonitor, UINT dpi);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::vector<AltTabListRow> rows_;
    std::optional<size_t> highlightIndex_;
    std::function<void(HWND)> onRowActivated_;
};

}  // namespace polish
