#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "windowtracking/GroupState.h"

namespace polish {

// The visible window for a group: a real, normal top-level application
// window (WS_OVERLAPPEDWINDOW, no WS_EX_TOOLWINDOW) with a self-painted
// title bar (replacing the native OS caption -- see TitleBarHeight) and,
// in Tab mode, a tab strip across the top listing its members -- unlike
// AltTabDimOverlay/AltTabHighlightBorder, this is meant to appear
// completely normally in the taskbar and Alt+Tab, both natively and via
// Polish's own replacement (see PLAN.md's "Alt+Tab integration" note:
// this needs zero special-casing in IsCandidateWindow/
// RebuildAltTabCandidates, simply by being a normal window).
//
// The custom title bar deliberately does NOT drop WS_CAPTION/
// WS_THICKFRAME/WS_SYSMENU (the window creation style is unchanged from
// a plain WS_OVERLAPPEDWINDOW) -- it shrinks the native caption's
// non-client space down to a thin resize-border sliver via WM_NCCALCSIZE
// and repaints its own content into the client area that frees up,
// reporting HTCAPTION/HTMINBUTTON/HTMAXBUTTON/HTCLOSE for the
// corresponding regions via WM_NCHITTEST. This is the same technique
// Windows Terminal's non-client island window uses, and it's what keeps
// DWM's drop shadow, Windows 11's rounded corners, WS_THICKFRAME edge-
// resize, and Aero Snap (drag-to-edge, Win+Arrow, and -- because
// HTMAXBUTTON is a real hit-test code Windows 11 itself recognizes --
// the Snap Layouts hover flyout) all working exactly as they would for a
// native caption, with none of it reimplemented by hand. Actually
// dropping WS_CAPTION entirely (a fully borderless WS_POPUP window) was
// considered and rejected for exactly this reason: it would silently
// lose all of the above and require reimplementing each one manually.
//
// Rendering is plain GDI (FillRect/Rectangle/DrawTextW), not the
// alpha-blended DIB/GDI+ pipeline the dim overlay and highlight border
// use -- that pipeline exists to solve a translucent-gradient problem
// this flat, fully opaque tab strip doesn't have.
//
// This class owns no group/window-positioning logic itself (that's
// GroupManager, driven from main.cpp) -- it only renders and reports
// input back to its owner via callbacks: which tab was clicked, a tab
// dragged to a new position, a mode-switch or edit-membership request
// from the right-click context menu, and when the window is resized
// (so the owner can re-lay-out members to fill the new content area --
// members are real children now, so they already move for free when
// the chrome itself moves; only a *resize* needs an explicit relayout).
class GroupChromeWindow {
public:
    explicit GroupChromeWindow(HINSTANCE instance);
    ~GroupChromeWindow();

    GroupChromeWindow(const GroupChromeWindow&) = delete;
    GroupChromeWindow& operator=(const GroupChromeWindow&) = delete;

    // Creates the window (first call only) and (re)renders its header
    // from `memberTitles`, then shows it. In Tab mode, the header is a
    // clickable/draggable tab strip, one tab per entry. In Tile mode
    // there's nothing to click (every member is simultaneously visible,
    // positioned by GroupManager's grid layout instead) -- the header
    // is just a plain label, and ComputeTabRects/hit-testing naturally
    // no-op. `mode` here is just the *initial* mode -- see SetMode for
    // runtime switching.
    void Show(const std::vector<std::wstring>& memberTitles, GroupMode mode = GroupMode::Tab);

    // Updates the tab strip's labels in place (no window re-creation/
    // re-show) and repaints -- used to reflect a member's title
    // changing live, or membership changing after an edit.
    void SetMemberTitles(const std::vector<std::wstring>& titles);

    // Updates the tab strip's icons in place and repaints. `icons` is
    // parallel to the titles/members list; a null entry means "no icon
    // for this tab" (falls back to text-only, unchanged layout). Icon
    // handles are borrowed from their owning window/class (via
    // WM_GETICON/GCLP_HICONSM) -- this class never destroys them.
    void SetMemberIcons(const std::vector<HICON>& icons);

    // Updates which tab is drawn as active (highlighted) and repaints.
    // Purely visual -- does not itself move/promote any member window.
    void SetActiveIndex(size_t index);

    // Switches the header between a Tab-mode tab strip and a Tile-mode
    // plain label, and repaints. Purely visual/input-handling -- the
    // caller is responsible for updating GroupState's own mode and
    // re-applying layout (GroupManager::ApplyLayout) afterward.
    void SetMode(GroupMode mode);

    // The area below the tab strip, in screen coordinates -- used by
    // GrowContentAreaTo to measure the chrome's current content size.
    RECT ContentRectInScreenCoords() const;

    // The area below the tab strip, in coordinates relative to this
    // window's own client origin -- what GroupManager::ApplyLayout
    // needs, since members are real children now and a child's
    // SetWindowPos x/y are relative to its parent's client area, not
    // the screen.
    RECT ContentRectInClientCoords() const;

    // Repaints just the tab strip band, not the whole client area --
    // for callers that only need the tab strip's own look refreshed
    // (e.g. the active tab's highlight after a hover-preview popup
    // closes). Deliberately narrower than a plain InvalidateRect(...,
    // nullptr, ...): invalidating the whole window also repaints the
    // content-area band PaintTabStrip fills behind the active member,
    // which visibly overwrites that member until it repaints itself
    // again -- a real, confirmed regression (File Explorer's content
    // going blank after moving the mouse off a tab).
    void InvalidateTabStrip();

    // Resizes the chrome window (top-left held fixed) so its content
    // area is at least `minContentSize`, if it isn't already. Exists
    // for when GroupManager::ApplyLayout reports that some member
    // refused to shrink to fit (a real, confirmed case -- e.g. Outlook
    // has a minimum size larger than a freshly-created group's default
    // chrome) -- growing the chrome and reapplying layout keeps that
    // member fully visible instead of spilling outside the group.
    // No-op if the content area is already big enough.
    void GrowContentAreaTo(SIZE minContentSize);

    HWND Handle() const { return window_; }

    // Called with the clicked tab's index on a left-click inside the
    // tab strip that didn't end up dragging it elsewhere (or, if it
    // was dragged, with its final index after the drop -- dragging a
    // tab also activates it, same as most browsers).
    void SetOnTabClicked(std::function<void(size_t)> callback) { onTabClicked_ = std::move(callback); }

    // Called with (fromIndex, toIndex) each time a dragged tab crosses
    // into a different tab's slot -- fires live during the drag (once
    // per crossing), not just once on drop, so the owner's GroupState
    // and this window's own label order stay in sync throughout.
    void SetOnTabReordered(std::function<void(size_t, size_t)> callback) { onTabReordered_ = std::move(callback); }

    // Called when "Switch to Tab"/"Switch to Tile" is chosen from the
    // right-click context menu, or the title bar's mode-toggle button is
    // clicked -- same request either way. The owner decides the actual
    // new mode (GroupState::Mode() is the source of truth) and calls
    // SetMode back.
    void SetOnModeToggleRequested(std::function<void()> callback) { onModeToggleRequested_ = std::move(callback); }

    // Called when "Edit windows..." is chosen from the right-click
    // context menu, or the title bar's manage-windows button is clicked.
    void SetOnEditWindowsRequested(std::function<void()> callback) {
        onEditWindowsRequested_ = std::move(callback);
    }

    // Called synchronously on WM_CLOSE, before the default handling
    // (DefWindowProc) destroys the window -- members are real children
    // now, so the owner MUST release them here (restore to top-level)
    // or they'd be destroyed along with this window. Covers every way
    // a user can close it: the X button, Alt+F4, "Close window" from
    // the taskbar -- all of those send WM_CLOSE, not WM_DESTROY
    // directly.
    void SetOnClosing(std::function<void()> callback) { onClosing_ = std::move(callback); }

    // Called whenever the chrome's client size actually changes
    // (WM_SIZE), so the owner can re-lay-out members to fill the new
    // content area. Not fired on a pure move -- children already move
    // for free with their parent, so a plain drag needs no callback at
    // all now (contrast the old reposition-only design's WM_MOVING-
    // driven follow logic, no longer needed).
    void SetOnResized(std::function<void()> callback) { onResized_ = std::move(callback); }

    // Called after the mouse rests on a tab for a short delay, with
    // that tab's index and its rect in *screen* coordinates (so the
    // owner can position a preview popup relative to it without this
    // class needing to know anything about previews itself) -- or with
    // std::nullopt when the hover ends (mouse moved off all tabs, or
    // left the window). Debounced internally (WM_MOUSEMOVE fires
    // continuously; this only fires on a genuine hover-target change,
    // after the delay, and once on leaving).
    void SetOnTabHovered(std::function<void(std::optional<size_t>, const RECT&)> callback) {
        onTabHovered_ = std::move(callback);
    }

    // Tile mode only: the draggable resize splitters' positions --
    // content-rect-relative pixels, matching
    // GroupManager::TileColumnBoundaries/TileRowBoundaries exactly (call
    // this again after every reflow, since a member added/removed or a
    // window resize can shift them). Repaints. A No-op boundary list
    // (e.g. a single-column/row grid) simply renders no splitters.
    void SetTileSplitters(std::vector<int> columnBoundaries, std::vector<int> rowBoundaries);

    // Called live while a splitter is being dragged (once per mouse-
    // move, not just on drop) with which boundary (column vs. row,
    // index into GroupManager::TileColumnBoundaries/TileRowBoundaries)
    // and its new content-rect-relative pixel position -- already
    // clamped so neither adjacent tile shrinks below this window's own
    // visible-content floor (kMinTileSize). The owner is responsible
    // for calling GroupManager::SetTileBoundary and re-applying layout.
    void SetOnTileSplitterDragged(std::function<void(bool column, size_t index, int newPixelPosition)> callback) {
        onTileSplitterDragged_ = std::move(callback);
    }

    // Called when a splitter is double-clicked -- the owner toggles it
    // between an even 50/50 split and whatever custom split it had
    // before, so a quick double-click undoes a drag without having to
    // eyeball it back into place.
    void SetOnTileSplitterDoubleClicked(std::function<void(bool column, size_t index)> callback) {
        onTileSplitterDoubleClicked_ = std::move(callback);
    }

    // Tile mode only: this window's current DPI-scaled splitter width,
    // in real pixels -- pass to GroupManager::ApplyLayout's
    // tileSplitterWidthPx and SetTileBoundary's splitterWidthPx so the
    // reserved-gap math on both sides always agrees. 0 before the
    // window exists.
    int TileSplitterWidthPx() const;

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void PaintTabStrip(HDC hdc, const RECT& clientRect);
    std::vector<RECT> ComputeTabRects(const RECT& clientRect) const;
    void ShowContextMenu(int screenX, int screenY);

    // Tile mode only. Returns (isColumn, index) for the splitter within
    // hit-test slop of `clientPt` (client coordinates), or nullopt.
    std::optional<std::pair<bool, size_t>> HitTestSplitter(POINT clientPt) const;
    // Clamps a dragged splitter's new position so neither of its two
    // adjacent tiles shrinks below kMinTileSize, then fires
    // onTileSplitterDragged_.
    void DragSplitter(POINT clientPt);
    // Repaints a narrow band around one splitter (its current cached
    // position) -- not the whole window, which would also repaint the
    // content-area fill behind the members. No-op if `index` is out of
    // range for the current boundary list.
    void InvalidateSplitterBand(bool column, size_t index);
    // Repaints just the title bar band (not the tab strip below it, and
    // not the content area) -- for a hover-highlight change on one of
    // the three caption buttons, same narrow-invalidate reasoning as
    // InvalidateTabStrip.
    void InvalidateTitleBar();

    // Total space reserved above the member content: the custom title
    // bar band (see TitleBarHeight) always, plus -- Tab mode only -- the
    // tab strip and its connector band below that. Tile mode has no tab
    // strip (nothing to click, every member is simultaneously visible)
    // but still needs the title bar band itself, since that's now the
    // only place the group's name and window controls are shown at all
    // (the native caption that used to show them is gone -- see
    // TitleBarHeight's own comment).
    int HeaderHeight(UINT dpi) const;

    // Height of the self-painted title bar band that replaces the native
    // OS caption (removed via WM_NCCALCSIZE shrinking the real
    // non-client caption down to a thin resize-border sliver -- see
    // HandleMessage's WM_NCCALCSIZE/WM_NCHITTEST cases). Always reserved,
    // in both Tab and Tile mode.
    int TitleBarHeight(UINT dpi) const;

    // Draws the icon, title text, and minimize/maximize-restore/close
    // button glyphs into the title bar band -- called from PaintTabStrip
    // (same memDC, same double-buffered BitBlt) so the whole client area
    // still repaints as one atomic frame.
    void PaintTitleBar(HDC hdc, const RECT& clientRect) const;
    // The three button rects (client coordinates), right-aligned within
    // the title bar band -- one shared source of truth for painting,
    // WM_NCHITTEST, and hover tracking, in that native left-to-right
    // order (minimize, maximize, close) matching Windows' own caption.
    RECT MinimizeButtonRect(const RECT& clientRect, UINT dpi) const;
    RECT MaximizeButtonRect(const RECT& clientRect, UINT dpi) const;
    RECT CloseButtonRect(const RECT& clientRect, UINT dpi) const;

    // Two more buttons immediately left of the caption buttons -- mode
    // toggle and manage-windows, the same two actions the right-click
    // context menu already offers (see ShowContextMenu), now with a
    // visible trigger instead of only a hidden menu. Unlike the caption
    // buttons above, these are ordinary *client*-area buttons (plain
    // WM_LBUTTONDOWN, not a WM_NCHITTEST code) -- there's no OS-
    // recognized hit-test value for "app-defined title bar button", so
    // WM_NCHITTEST instead reports plain HTCLIENT for these two rects
    // (see its own switch) rather than HTCAPTION, letting normal client
    // mouse messages reach them.
    RECT ModeToggleButtonRect(const RECT& clientRect, UINT dpi) const;
    RECT ManageWindowsButtonRect(const RECT& clientRect, UINT dpi) const;

    // Shows/hides/repositions tooltipWindow_ for whichever title-bar
    // button (caption or client-area) is currently hovered -- called
    // from every place hoveredTitleBarButton_/hoveredActionButton_
    // changes (WM_NCMOUSEMOVE/WM_NCMOUSELEAVE, the client-area hover
    // block in WM_MOUSEMOVE, WM_MOUSELEAVE), plus right after a button's
    // action fires so text that depends on the new state (Maximize vs
    // Restore, Switch to Tile vs Switch to Tab) is correct immediately
    // if the cursor is still sitting on the same button post-click.
    void UpdateTooltip();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    // A manually-tracked (TTF_TRACK) tooltip, not the common controls'
    // usual auto-relay-mouse-messages mode -- this window's own hover
    // state already gets tracked for the hover-fill paint, so driving
    // the tooltip from those same state changes (see UpdateTooltip) is
    // simpler than trying to relay non-client mouse messages (the
    // caption buttons') into the tooltip's own automatic tracking,
    // which isn't designed for WM_NCMOUSEMOVE at all. Owned by window_
    // (a WS_POPUP with window_ as its CreateWindowExW owner param), so
    // it's destroyed automatically when window_ is.
    HWND tooltipWindow_ = nullptr;
    std::vector<std::wstring> memberTitles_;
    std::vector<HICON> memberIcons_;  // borrowed handles, never destroyed here
    GroupMode mode_ = GroupMode::Tab;
    size_t activeIndex_ = 0;
    std::optional<size_t> draggingIndex_;
    std::function<void(size_t)> onTabClicked_;
    std::function<void(size_t, size_t)> onTabReordered_;
    std::function<void()> onModeToggleRequested_;
    std::function<void()> onEditWindowsRequested_;
    std::function<void()> onResized_;
    std::function<void()> onClosing_;
    std::function<void(std::optional<size_t>, const RECT&)> onTabHovered_;
    std::optional<size_t> hoveredTabIndex_;
    bool trackingMouseLeave_ = false;

    // Which title-bar button (HTMINBUTTON/HTMAXBUTTON/HTCLOSE) the mouse
    // is currently over, if any -- nullopt otherwise. Drives only the
    // hover highlight PaintTitleBar draws; the actual minimize/maximize/
    // close action is handled explicitly in WM_NCLBUTTONDOWN, keyed off
    // the same codes -- confirmed live (message-level logging) that
    // DefWindowProcW's own default handling neither performs these
    // automatically nor even delivers a WM_NCLBUTTONUP back to this
    // window's own WndProc for these specific hit-test codes, once the
    // real caption has been shrunk to a thin border the way this class
    // does -- its own internal button-tracking loop appears to consume
    // the eventual release. Handling on *down* is what actually works,
    // even though committing on the native caption's own *up* would
    // normally be the more correct-feeling choice.
    std::optional<UINT> hoveredTitleBarButton_;
    // Separate from trackingMouseLeave_ (client-area tab hover) --
    // TME_NONCLIENT is its own TrackMouseEvent registration, needed to
    // reliably get WM_NCMOUSELEAVE once the cursor leaves a title-bar
    // button, the non-client counterpart of the same pattern.
    bool trackingNcMouseLeave_ = false;

    // Which of the two *client*-area title-bar buttons (mode toggle,
    // manage windows) the mouse is over, if any -- the client-area
    // counterpart of hoveredTitleBarButton_ above, tracked via ordinary
    // WM_MOUSEMOVE/WM_MOUSELEAVE (trackingMouseLeave_, already armed for
    // tab hover) rather than the NC variants, since these aren't a
    // WM_NCHITTEST code. Drives only the hover highlight; both buttons
    // fire their callback directly on WM_LBUTTONDOWN (see there) with no
    // separate pressed-state tracking, matching the caption buttons'
    // own commit-on-down choice, and simpler than tabs' drag/capture
    // handling for something that's just a one-shot command.
    enum class TitleBarActionButton { ModeToggle, ManageWindows };
    std::optional<TitleBarActionButton> hoveredActionButton_;

    // Tile mode splitters -- content-rect-relative pixel positions, set
    // by SetTileSplitters after every reflow.
    std::vector<int> tileColumnBoundaries_;
    std::vector<int> tileRowBoundaries_;
    std::function<void(bool, size_t, int)> onTileSplitterDragged_;
    std::function<void(bool, size_t)> onTileSplitterDoubleClicked_;
    // (isColumn, index) of the splitter currently being dragged, if
    // any -- mutually exclusive with draggingIndex_ (Tab-mode tab drag)
    // since a chrome is only ever in one mode at a time.
    std::optional<std::pair<bool, size_t>> draggingSplitter_;
    // (isColumn, index) of the splitter currently under the cursor
    // (not necessarily being dragged) -- drawn in the accent color so
    // it reads as draggable, same idea as a browser's own resize
    // handles.
    std::optional<std::pair<bool, size_t>> hoveredSplitter_;
};

}  // namespace polish
