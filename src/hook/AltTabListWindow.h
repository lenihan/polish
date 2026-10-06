#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace polish {

// One row in the panel -- icon and title are captured at Show() time
// (borrowed HICON, same "never destroyed here" contract as
// StackStripWindow::SetTabs) since the underlying window could
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
// edge (M5): close, minimize/normal-toggle and maximize/normal-toggle.
// All three on every row, minimized ones included. The maximize button
// used to be left off a minimized row, reasoning that a minimized window
// has no maximized state to toggle -- but that confused "what state is
// it in" with "where can it go": a minimized window is coming back one
// way or the other, and the two buttons are how you say at which size.
// Clicking any of them acts on that row's window without
// ending the session or moving Tab's own highlight; every row (not just
// a highlighted/hovered one) reserves the same icon-sized space at its
// right edge regardless, so row text never reflows width as
// highlight/hover moves around or a row's own button count differs. A
// footer legend pinned below everything else documents the Del/-/+
// keyboard equivalents (see AltTabHook::RowAction) so they don't have to
// be discovered by accident. If a monitor has enough candidates that the
// full list would overflow its work area, the panel's height is capped
// instead of growing past the screen, and the content scrolls (see
// scrollOffset_) to keep the highlighted row in view, with a small
// chevron+count strip (e.g. "▾ 12 more") marking whichever edge(s)
// still have hidden content.
//
// On a multi-monitor setup, per explicit user request, one instance of
// this class exists *per connected monitor* (see main.cpp's
// g_altTabPanels) rather than a single panel following the cursor/
// foreground window around: every monitor shows its own subset of the
// full candidate list (which windows land in a given monitor's subset is
// decided by the caller, not this class), and only the one monitor whose
// subset actually contains the globally-highlighted window shows a
// highlighted row -- the rest show `std::nullopt` (no highlight). The
// underlying candidate order stacks the current monitor's windows first,
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
// StackPickerWindow's teardown-per-show/nested-message-loop pattern.
//
// Rendering is plain, flat-opaque GDI (double-buffered WM_PAINT, the same
// technique StackStripWindow's tab strip uses) -- not the DIB+GDI+
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
    // "Running windows"; the caller sets an app's name while a session is
    // scoped to one app (Alt+`), so the panel says what it is showing --
    // and must set it back when the scope widens. Takes effect on the
    // next Show() (which lays out and paints from scratch), so call it
    // before Show() rather than expecting an already-visible panel to
    // update. A setter rather than a Show() parameter because the cheap
    // SetHighlight path never needs it.
    void SetActiveSectionHeader(std::wstring text) { activeHeader_ = std::move(text); }

    // What that heading says when the list is not scoped to one app.
    //
    // A named constant because two separate places in main.cpp set the
    // header back to it after an Alt+` session ends, and a bare literal
    // in each meant the default and the reset could disagree -- as they
    // briefly did, leaving a panel that said "Running windows" until the
    // first Alt+` and "Active" ever after.
    static constexpr const wchar_t* kDefaultActiveHeader = L"Running windows";

    // Whether rows carry their per-row minimize/maximize/close buttons.
    // Each one names its own keyboard shortcut in its tooltip; there is
    // no separate legend.
    //
    // Switched off for an Alt+` tab session, whose rows stand for tabs
    // rather than windows: a tab has no HWND, so every one of those
    // actions is meaningless for it, and the row would otherwise reserve
    // width for buttons it never draws. Takes effect on the next Show().
    void SetRowActionsEnabled(bool enabled);


    // A command row drawn above the window list, or empty for none.
    //
    // Not an AltTabListRow with a flag on it: a row stands for a window,
    // and everything here that walks rows -- the highlight, the hover,
    // the per-row buttons, the minimized partition -- would then have to
    // special-case an entry with no HWND. Keeping it outside the row list
    // means none of them need to know it exists.
    //
    // `mnemonic` is drawn after the text but never bound here: this
    // window has no keyboard focus of its own (see the class comment), so
    // the owner binds the key and this only advertises it.
    void SetCommandRow(std::wstring text, wchar_t mnemonic);
    void SetOnCommandRow(std::function<void()> callback) { onCommandRow_ = std::move(callback); }

    // Pins the panel just outside `anchor` (a screen rect in physical
    // pixels) instead of centering it on the monitor.
    //
    // The taskbar hover panel has to point at the button it belongs to --
    // a panel centered on the monitor would leave the user to work out
    // which app it was describing. Centered on the anchor horizontally,
    // clamped to stay on screen, and placed on whichever side of it has
    // room, so it works for a taskbar at the bottom, the top or either
    // edge without knowing which it is looking at.
    //
    // nullopt restores the monitor-centered placement Alt+Tab uses.
    // Takes effect on the next Show(), like the other layout setters.
    void SetAnchorRect(std::optional<RECT> anchor) { anchorRect_ = anchor; }

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

    // Fired by a click on a row's maximize/normal-toggle icon button.
    // On a minimized row it always means "come back maximized" -- the
    // button beside it is the one that means "come back normal". Same
    // non-committing, non-relocating contract as SetOnRowMinimizeToggle
    // otherwise.
    void SetOnRowMaximizeToggle(std::function<void(HWND)> callback) { onRowMaximizeToggle_ = std::move(callback); }

    // Fired when the mouse moves onto a different row, with that row's
    // HWND -- or nullptr when it leaves the rows entirely. Hover already
    // reveals a row's action buttons; this exists so a caller can react
    // to it too, which is what lets the taskbar hover panel halo the real
    // window on screen as each row is pointed at.
    //
    // Fires on the change only, not per mouse-move, and never for a row
    // whose HWND is null (an Alt+` tab row).
    void SetOnRowHovered(std::function<void(HWND)> callback) { onRowHovered_ = std::move(callback); }

    // The window whose row the mouse is currently over, or nullptr.
    //
    // Exposed so the row-action shortcuts can act on what the user is
    // pointing at rather than on whatever is selected -- the point of
    // having them is to manage a window without first having to click it
    // and make it current. Already nullptr while the panel is hidden:
    // Hide() forgets the hovered row precisely so a stale one cannot
    // outlive the panel it belonged to.
    HWND HoveredRowWindow() const;

    // Which of a row's three action buttons the pointer is over, if any.
    // Tracked so the button under the cursor can light up and name itself
    // the way a real title-bar button does -- without it, three unlabeled
    // glyphs appear on hover and give no feedback at all about which one
    // is about to be clicked.
    enum class ActionButton { Close, MinimizeToggle, MaximizeToggle };

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
        // The command row, when there is one -- above everything, and
        // never part of rowRects (see SetCommandRow).
        std::optional<RECT> commandRect;
        std::optional<RECT> activeHeaderRect;
        std::optional<RECT> minimizedHeaderRect;
        // Headers + rows, which is everything the panel draws.
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
    // Which action button `pt` (in unshifted content coordinates) falls
    // on, for whichever rows currently draw them.
    void UpdateHoveredAction(POINT pt, UINT dpi, const std::vector<RECT>& rowRects);
    // Shows the hovered button's name, or hides the tip when none.
    void UpdateActionTooltip();
    // Re-derives scrollOffset_ from the current window's actual client
    // height versus ComputeLayout's natural (unclamped) content height,
    // keeping the highlighted row in view with the smallest possible
    // scroll (an "ensure visible" listbox-style scroll, not a re-center-
    // every-time one) -- see class comment on overflow. Returns whether
    // scrollOffset_ actually changed value, so a caller that only moved
    // the highlight within the already-visible viewport can still take
    // its own cheap narrow-invalidate path instead of a full repaint.
    bool RecomputeScrollOffset();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::vector<AltTabListRow> rows_;
    // "Running", not "Active": only one window is ever active -- the one
    // with focus -- and this section lists every window that is merely
    // running, which is a different thing. The old wording said the list
    // was full of active windows while marking exactly one of them as
    // selected, contradicting itself.
    std::wstring activeHeader_ = kDefaultActiveHeader;  // see SetActiveSectionHeader
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
    // The row and button the pointer is over, together -- a button only
    // means something paired with the row it belongs to, and both change
    // at once.
    std::optional<size_t> hoveredActionRow_;
    std::optional<ActionButton> hoveredAction_;
    // Tracked tooltip for the hovered action button (TTF_TRACK, driven
    // by this class rather than by the tooltip's own mouse relay -- the
    // same approach StackStripWindow uses for its title-bar buttons).
    HWND tooltipWindow_ = nullptr;

    bool rowActionsEnabled_ = true;      // see SetRowActionsEnabled
    std::optional<RECT> anchorRect_;     // see SetAnchorRect
    std::wstring commandText_;           // see SetCommandRow; empty = none
    wchar_t commandMnemonic_ = 0;
    bool commandHovered_ = false;
    std::function<void()> onCommandRow_;
    std::function<void(HWND)> onRowActivated_;
    std::function<void(HWND)> onRowMinimizeToggle_;
    std::function<void(HWND)> onRowMaximizeToggle_;
    std::function<void(HWND)> onRowClose_;
    std::function<void(HWND)> onRowHovered_;
};

}  // namespace polish
