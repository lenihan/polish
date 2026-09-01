#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "windowtracking/GroupState.h"

namespace polish {

// The visible window for a group: a real, normal top-level application
// window (WS_OVERLAPPEDWINDOW, no WS_EX_TOOLWINDOW) with a tab strip
// across the top listing its members -- unlike AltTabDimOverlay/
// AltTabHighlightBorder, this is meant to appear completely normally in
// the taskbar and Alt+Tab, both natively and via Polish's own
// replacement (see PLAN.md's "Alt+Tab integration" note: this needs
// zero special-casing in IsCandidateWindow/RebuildAltTabCandidates,
// simply by being a normal window).
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
// from the right-click context menu, and when the window itself is
// being moved (so the owner can re-drive member windows to follow in
// lockstep -- see PLAN.md's group-dragging mechanism, no SetParent
// involved).
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

    // Updates which tab is drawn as active (highlighted) and repaints.
    // Purely visual -- does not itself move/promote any member window.
    void SetActiveIndex(size_t index);

    // Switches the header between a Tab-mode tab strip and a Tile-mode
    // plain label, and repaints. Purely visual/input-handling -- the
    // caller is responsible for updating GroupState's own mode and
    // re-applying layout (GroupManager::ApplyLayout) afterward.
    void SetMode(GroupMode mode);

    // The area below the tab strip, in screen coordinates -- where
    // GroupManager should position every member window.
    RECT ContentRectInScreenCoords() const;

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
    // right-click context menu. The owner decides the actual new mode
    // (GroupState::Mode() is the source of truth) and calls SetMode
    // back.
    void SetOnModeToggleRequested(std::function<void()> callback) { onModeToggleRequested_ = std::move(callback); }

    // Called when "Edit windows..." is chosen from the right-click
    // context menu.
    void SetOnEditWindowsRequested(std::function<void()> callback) {
        onEditWindowsRequested_ = std::move(callback);
    }

    // Called synchronously, once per WM_WINDOWPOSCHANGING where the
    // window's position is actually changing (i.e. mid-drag, once per
    // incremental move), with the content rect (screen coordinates) the
    // window is about to have -- the *proposed* new position, since at
    // WM_WINDOWPOSCHANGING time the window itself hasn't actually moved
    // yet (querying its current on-screen position here would give the
    // stale, pre-move rect and members would visibly lag a frame behind
    // during a drag). The owner re-drives member windows to follow from
    // here, not via SetParent.
    void SetOnMoved(std::function<void(const RECT&)> callback) { onMoved_ = std::move(callback); }

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void PaintTabStrip(HDC hdc, const RECT& clientRect);
    std::vector<RECT> ComputeTabRects(const RECT& clientRect) const;
    void ShowContextMenu(int screenX, int screenY);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::vector<std::wstring> memberTitles_;
    GroupMode mode_ = GroupMode::Tab;
    size_t activeIndex_ = 0;
    std::optional<size_t> draggingIndex_;
    std::function<void(size_t)> onTabClicked_;
    std::function<void(size_t, size_t)> onTabReordered_;
    std::function<void()> onModeToggleRequested_;
    std::function<void()> onEditWindowsRequested_;
    std::function<void(const RECT&)> onMoved_;
};

}  // namespace polish
