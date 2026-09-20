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
// improvements M4). Any row -- highlighted or, per explicit user request,
// merely hovered by the mouse -- shows small icon buttons at its right
// edge (M5): close and minimize/restore-toggle always, plus a third
// maximize/restore-toggle button on active-section rows only (there's no
// meaningful "maximized" state to toggle for a window that's currently
// minimized). Clicking any of them acts on that row's window without
// ending the session or moving Tab's own highlight; every row (not just
// a highlighted/hovered one) reserves the same icon-sized space at its
// right edge regardless, so row text never reflows width as
// highlight/hover moves around or a row's own button count differs. A
// footer legend pinned below everything else documents the Del/-/+
// keyboard equivalents (see AltTabHook::RowAction) so they don't have to
// be discovered by accident. If a monitor has enough candidates that the
// full list would overflow its work area, the panel's height is capped
// instead of growing past the screen, and the headers/rows scroll (see
// scrollOffset_) to keep the highlighted row in view -- the footer legend
// stays pinned in place regardless, and a small chevron+count strip
// (e.g. "▾ 12 more") marks whichever edge(s) still have hidden content.
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

    // Forces a repaint of whatever row currently displays hwnd, without
    // touching row content, layout, or the highlight/hover indices. For a
    // change that alters how an existing row's own action-button glyph
    // should look (e.g. the maximize/restore-toggle glyph, which reflects
    // live IsZoomed(row.hwnd) state read fresh at paint time) without
    // changing the candidate list or which row is highlighted/hovered --
    // neither of which Show()/SetHighlight()'s own change-detection would
    // otherwise catch, since by both of those measures nothing changed.
    // No-op if hwnd isn't currently one of this panel's rows.
    void RepaintRow(HWND hwnd);

    // The text of the heading above the non-minimized rows. Defaults to
    // "Active"; the caller sets an app's name while a session is scoped to
    // one app (Alt+`), so the panel says what it is showing -- and must
    // set it back to "Active" when the scope widens. Takes effect on the
    // next Show() (which lays out and paints from scratch), so call it
    // before Show() rather than expecting an already-visible panel to
    // update. A setter rather than a Show() parameter because the cheap
    // SetHighlight path never needs it.
    void SetActiveSectionHeader(std::wstring text) { activeHeader_ = std::move(text); }

    // Whether rows carry their per-row minimize/maximize/close buttons
    // and the keyboard-shortcut footer legend that documents them.
    //
    // Switched off for an Alt+` tab session, whose rows stand for tabs
    // rather than windows: a tab has no HWND, so every one of those
    // actions is meaningless for it, and the row would otherwise reserve
    // width for buttons it never draws. Takes effect on the next Show().
    void SetRowActionsEnabled(bool enabled);

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

    // Fired by a click on a row's minimize/restore-toggle icon button
    // (see class comment) -- only ever hit-testable on the highlighted
    // row or the currently mouse-hovered row (never any other). Does NOT
    // end the session or call onRowActivated_; the caller is expected to
    // act on hwnd (minimize if active, restore if minimized) and
    // immediately refresh this panel's content in place.
    void SetOnRowMinimizeToggle(std::function<void(HWND)> callback) { onRowMinimizeToggle_ = std::move(callback); }

    // Fired by a click on an active-section row's maximize/restore-toggle
    // icon button -- never drawn or hit-testable on a minimized-section
    // row (see class comment). Same non-committing, non-relocating
    // contract as SetOnRowMinimizeToggle otherwise.
    void SetOnRowMaximizeToggle(std::function<void(HWND)> callback) { onRowMaximizeToggle_ = std::move(callback); }

    // Fired by a click on a row's close ("X") icon button, same
    // highlighted-or-hovered-only contract as SetOnRowMinimizeToggle. The
    // caller posts a close request to hwnd and leaves ending/continuing
    // the session up to whatever happens as a result.
    void SetOnRowClose(std::function<void(HWND)> callback) { onRowClose_ = std::move(callback); }

private:
    // "Active"/"Minimized" section headings sit above their respective
    // rows -- computed alongside row layout, not as separate rows
    // themselves, so they're never mistaken for a clickable/highlightable
    // row by WM_LBUTTONDOWN or SetHighlight (both only ever look at
    // rowRects). Either optional is unset when that section has no rows
    // to head (e.g. a monitor whose panel only has minimized candidates
    // has no activeHeaderRect).
    struct RowLayout {
        std::vector<RECT> rowRects;
        std::optional<RECT> activeHeaderRect;
        std::optional<RECT> minimizedHeaderRect;
        // Headers + rows only -- the keyboard-shortcut footer legend is
        // NOT part of this "natural" scrollable layout at all (see
        // Paint's own comment on FooterBandHeight): it's always pinned to
        // the bottom of the actual viewport instead, so it stays visible
        // even while this content scrolls underneath it.
        int contentHeight = 0;
    };

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& clientRect) const;
    // Single source of truth for panel layout -- Reposition, Paint, and
    // WM_LBUTTONDOWN/SetHighlight's row hit-testing all derive their rects
    // from this one pass rather than each recomputing overlapping layout
    // math that could quietly drift out of sync with each other.
    RowLayout ComputeLayout(UINT dpi) const;
    // Resizes/repositions the panel (content-sized from the current row
    // count) centered on `targetMonitor`, at the given DPI.
    void Reposition(HMONITOR targetMonitor, UINT dpi);
    // Updates hoveredIndex_ (nullopt to clear) and narrow-invalidates
    // just the old/new hovered row -- same shape as SetHighlight's own
    // cheap-repaint path, for the same reason (avoid a full-panel
    // repaint on every mouse-move over the list).
    void SetHoveredIndex(std::optional<size_t> index);
    // Re-derives scrollOffset_ from the current window's actual client
    // height versus ComputeLayout's natural (unclamped) content height,
    // keeping the highlighted row in view with the smallest possible
    // scroll (an "ensure visible" listbox-style scroll, not a re-center-
    // every-time one) -- see class comment on overflow. Returns whether
    // scrollOffset_ actually changed value, so a caller that only moved
    // the highlight within the already-visible viewport can still take
    // its own cheap narrow-invalidate path instead of a full repaint.
    bool RecomputeScrollOffset();
    // FooterBandHeight, or zero when the legend is switched off -- the
    // single place that distinction is made, so layout, painting and
    // scrolling cannot disagree about how tall the viewport is.
    int FooterBandHeightForState(UINT dpi) const;

    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::vector<AltTabListRow> rows_;
    std::wstring activeHeader_ = L"Active";  // see SetActiveSectionHeader
    std::optional<size_t> highlightIndex_;
    // Vertical pixel offset applied only when there are more rows than
    // fit in the panel's height-capped viewport (see kViewportMarginPx in
    // the .cpp) -- 0 whenever everything fits, which is the overwhelming
    // common case. ComputeLayout's rects stay in this "natural",
    // unshifted coordinate space throughout; Paint applies the shift via
    // SetViewportOrgEx for drawing, and every other consumer of a
    // ComputeLayout rect (WM_LBUTTONDOWN/WM_MOUSEMOVE's hit-testing,
    // SetHighlight/SetHoveredIndex/RepaintRow's invalidate rects) applies
    // it manually instead, since none of those go through a GDI DC
    // transform. Follows only the highlighted row, never the hovered one
    // -- scrolling the list just because the mouse happened to sit near
    // an edge would be jarring, not helpful.
    int scrollOffset_ = 0;
    // The row currently under the mouse cursor, if any -- independent of
    // highlightIndex_ (Tab-cycling and hovering are unrelated: hovering
    // an unselected row reveals its own action buttons without touching
    // Tab's own selection). Reset to nullopt on WM_MOUSELEAVE, since plain
    // WM_MOUSEMOVE never fires once the cursor actually leaves the client
    // area -- TrackMouseEvent(TME_LEAVE) is what makes that message
    // arrive at all, re-armed on every WM_MOUSEMOVE since it's otherwise
    // a one-shot subscription per MSDN.
    std::optional<size_t> hoveredIndex_;
    bool rowActionsEnabled_ = true;  // see SetRowActionsEnabled
    std::function<void(HWND)> onRowActivated_;
    std::function<void(HWND)> onRowMinimizeToggle_;
    std::function<void(HWND)> onRowMaximizeToggle_;
    std::function<void(HWND)> onRowClose_;
};

}  // namespace polish
