#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace polish {

// One row: a candidate window not currently a stack member, its title,
// and its icon. Icon is captured once (in SetWindows) and borrowed --
// same "never destroyed here" contract as AltTabListRow::icon /
// StackStripWindow::SetTabs.
struct StackPickerRow {
    HWND hwnd = nullptr;
    std::wstring title;
    HICON icon = nullptr;  // borrowed, may be nullptr (falls back to text-only)
};

// The "Available windows" half of StackPickerWindow's picker: every
// candidate window *not currently selected*, in a fixed, never-reordered
// stable order -- a window disappears from this list the moment it's
// added to the stack (see the sibling StackPickerSelectedListWindow) and
// reappears here if later removed, the same mutually-exclusive-lists
// mechanics the dialog's original (pre-Windows-11-redesign) two-list
// model had.
//
// Each row is icon + title + a single inline "Add" arrow button at its
// right edge -- clicking that button (and only that button; the rest of
// the row does nothing) requests adding that window. This replaced an
// earlier checkbox-based version where clicking *anywhere* on a row
// toggled it: on this class's own sibling (the Selected panel), that
// same "click anywhere" convention made an imprecise drag attempt
// silently register as an accidental removal instead -- a real,
// human-reported problem. A single small, explicit, unambiguous button
// per action (mirroring AltTabListWindow's own reserved-space per-row
// icon buttons) has no such failure mode: every click either hits the
// one thing it can do, or hits nothing.
//
// The Add button is only ever drawn/hit-testable on the currently
// "selected" row (selectedIndex_, pushed down from StackPickerWindow's
// own single cross-list selectedWindow_ via SetSelectedHwnd -- see that
// class's own comment for why selection lives up there, not here)
// and/or whichever row the mouse currently hovers (hoveredIndex_,
// independent of selectedIndex_) -- the exact same "selected-or-hovered
// reveals per-row action buttons" mechanism AltTabListWindow already
// uses for its own close/minimize/maximize icons (see that class's own
// ComputeLayout/Paint/WM_LBUTTONDOWN for the shape being mirrored
// here), rather than showing every row's button all the time. Clicking
// a row's body (anywhere but the Add button) reports that row via
// onRowSelected_ instead of doing nothing -- StackPickerWindow is the
// one that actually updates selectedWindow_ and pushes it back down to
// both this list and its sibling, so this class never decides selection
// on its own.
//
// Double-clicking a row's body does what its Add button would --
// requires no more precision than a single click on the body (unlike
// clicking the small button itself), so it's a quicker way to move a
// window across once you already know which one you want. This doesn't
// reintroduce the single-click-ambiguity problem the button design
// above replaced: a double-click is already a distinct, deliberate
// gesture from a single click or a drag, so it can't be mistaken for
// either.
//
// Modeled on AltTabListWindow's ComputeLayout/Paint/hit-test/scroll
// shape (the closest existing precedent for a hand-rolled, DPI-aware,
// scrollable icon+text list in this codebase), but a plain WS_CHILD --
// opaque, normal z-order, created fresh in
// StackPickerWindow::CreateControls and destroyed with it -- not
// AltTabListWindow's WS_POPUP/WS_EX_LAYERED/topmost/created-once shape,
// which solves a different (translucent overlay) problem.
//
// Keyboard: this panel is one of StackPickerWindow's five Tab stops (see
// that class's CycleFocus). While it holds focus it draws an accent
// border of its own -- the only cue available when the list is empty and
// has no selected row to highlight -- and Up/Down move the selection
// within its own rows, reported up via the same onRowSelected_ callback a
// row-body click uses. Still unimplemented (not needed so far): Space/
// Enter to trigger the row's Add button, Home/End, PageUp/PageDown.
class StackPickerListWindow {
public:
    explicit StackPickerListWindow(HINSTANCE instance);
    ~StackPickerListWindow();

    StackPickerListWindow(const StackPickerListWindow&) = delete;
    StackPickerListWindow& operator=(const StackPickerListWindow&) = delete;

    // Creates the child window against `parent`. Caller positions it via
    // MoveWindow, same as any other child control.
    HWND Create(HWND parent);

    // Replaces the row list with `candidates`, in the given order (the
    // caller decides candidate order; this class never reorders it).
    void SetWindows(const std::vector<HWND>& candidates);

    // Fired when a row's Add button is clicked, with that row's hwnd.
    void SetOnAddRequested(std::function<void(HWND)> callback) { onAddRequested_ = std::move(callback); }

    // Fired when a row's body (anywhere but the Add button) is clicked,
    // with that row's hwnd -- StackPickerWindow owns what "selected"
    // actually means across both lists; this just reports the gesture.
    void SetOnRowSelected(std::function<void(HWND)> callback) { onRowSelected_ = std::move(callback); }

    // Updates which row (if any) renders as selected, matched by hwnd
    // identity against the current rows_ -- O(n), fine at this list's
    // size. Deliberately separate from SetWindows: does not touch
    // scrollOffset_/hoveredIndex_ or rebuild rows_, so a pure selection
    // change (a click, or a sibling-list move) never disturbs scroll
    // position or re-fetches icons.
    void SetSelectedHwnd(std::optional<HWND> hwnd);

    HWND WindowHandle() const { return window_; }

private:
    struct RowLayout {
        std::vector<RECT> rowRects;  // natural (unshifted-by-scroll) coordinates
        int contentHeight = 0;
    };

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& clientRect) const;
    // Single source of truth for row rects -- Paint and WM_LBUTTONDOWN/
    // WM_MOUSEMOVE hit-testing all derive from this one pass.
    RowLayout ComputeLayout(UINT dpi) const;
    void UpdateScrollInfo();
    void SetHoveredIndex(std::optional<size_t> index);
    // Shows/repositions/hides the "Add to stack" tooltip to match
    // whichever row (if any) currently has its Add button visible under
    // the cursor -- see AltTabListWindow's tooltip handling for the
    // identical TTF_TRACK-based technique this mirrors.
    void UpdateTooltip();
    // Scrolls just far enough to bring row `index` fully into view, if it
    // isn't already -- keyboard navigation can move the selection past
    // either edge of the viewport, which the mouse-driven paths never do.
    // One viewport's worth of rows, for PageUp/PageDown. Never 0.
    size_t RowsPerPage() const;
    void EnsureRowVisible(size_t index);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND tooltipWindow_ = nullptr;
    // Whether this panel currently owns keyboard focus -- drives its own
    // accent border (see Paint) and nothing else; which row is selected
    // still lives in selectedIndex_, pushed down from the parent.
    bool hasFocus_ = false;
    std::vector<StackPickerRow> rows_;
    // The row whose Add button is visible even without the mouse over
    // it -- set only via SetSelectedHwnd, pushed down from
    // StackPickerWindow's shared selectedWindow_ (see this class's own
    // comment on why). Independent of hoveredIndex_, same as
    // AltTabListWindow's highlightIndex_/hoveredIndex_ pair -- either
    // one alone is enough to reveal a row's button.
    std::optional<size_t> selectedIndex_;
    std::optional<size_t> hoveredIndex_;
    // Vertical pixel offset, only nonzero once there are more rows than
    // fit in the child's current height -- same "natural unshifted
    // ComputeLayout rects, SetViewportOrgEx at paint time, hit-testers
    // shift the point instead" pattern AltTabListWindow uses.
    int scrollOffset_ = 0;
    std::function<void(HWND)> onAddRequested_;
    std::function<void(HWND)> onRowSelected_;
};

}  // namespace polish
