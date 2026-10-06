#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace polish {

// One row in the Selected list -- title/icon looked up fresh in
// SetSelected (borrowed HICON, same "never destroyed here" contract as
// StackPickerRow::icon).
struct StackPickerSelectedRow {
    HWND hwnd = nullptr;
    std::wstring title;
    HICON icon = nullptr;
};

// The "Stack" half of StackPickerWindow's picker (side by side with the
// sibling StackPickerListWindow, "Available windows", the original
// pre-Windows-11-redesign layout this returns to): exactly the windows
// currently in the stack, in committed order, each row an icon + title
// + a Remove button (a left-pointing arrow back toward Available windows,
// the direction that window returns to) + a drag handle for reordering.
//
// Two distinct, explicit hit targets per row -- Remove and the grip --
// not a "click anywhere on the row" gesture for either. An earlier
// version of this class used exactly that (click body = remove, small
// grip zone = drag), and a mis-landed drag attempt (missing the grip by
// a few pixels) would silently register as an accidental removal
// instead -- a real, human-reported problem. A later version replaced
// the grip with explicit Move Up/Move Down buttons instead, which
// avoided that specific failure mode but was reported as clunkier than
// a drag handle for reordering felt. This keeps the explicit Remove
// button (so accidental removal from a near-missed drag still can't
// happen) while bringing the grip back for reordering -- a plain row-body
// click (neither rect) is a third, later-added gesture of its own:
// select this row (see SetOnRowSelected), never remove or drag it.
//
// A row-body *double*-click removes it, though -- unlike a single click,
// a double-click can't be an imprecise drag attempt (a drag is
// press-move-release, not two quick clicks in place), so it doesn't
// reintroduce the accidental-removal failure mode the paragraph above
// describes. It exists purely as a quicker path once you already know
// which window you want out, mirroring the sibling list's identical
// double-click-to-add.
//
// Remove and the grip are only ever drawn/hit-testable on the currently
// "selected" row (selectedIndex_, pushed down from StackPickerWindow's
// own single cross-list selectedWindow_) and/or whichever row the mouse
// currently hovers (hoveredIndex_) -- the identical selected-or-hovered
// reveal StackPickerListWindow's own Add button uses, so both lists
// behave the same way (see that class's own header comment for the
// AltTabListWindow precedent this mirrors).
//
// Owns no membership/order/selection state of its own beyond what was
// last pushed down -- StackPickerWindow is the single source of truth
// (its own selectedOrder_ and selectedWindow_), pushed down here via
// SetSelected/SetSelectedHwnd and reported back up via
// onReordered_/onRemoveRequested_/onRowSelected_ on every user gesture,
// the same unidirectional "parent owns state, children render + report
// gestures" shape StackPickerListWindow already uses.
//
// Modeled on StackPickerListWindow's own ComputeLayout/Paint/hit-test/
// scroll shape (ultimately AltTabListWindow's) -- a plain WS_CHILD,
// opaque, created fresh in StackPickerWindow::CreateControls.
//
// Keyboard: one of StackPickerWindow's five Tab stops, with the same
// focus-border/Up-Down-moves-selection behavior as its sibling (see
// StackPickerListWindow's own comment). Alt+Up/Alt+Down is the keyboard
// equivalent of dragging a row's grip -- Alt-modified specifically so
// plain Up/Down keeps meaning "move the selection" and can never
// silently rearrange the stack.
class StackPickerSelectedListWindow {
public:
    explicit StackPickerSelectedListWindow(HINSTANCE instance);
    ~StackPickerSelectedListWindow();

    StackPickerSelectedListWindow(const StackPickerSelectedListWindow&) = delete;
    StackPickerSelectedListWindow& operator=(const StackPickerSelectedListWindow&) = delete;

    // Creates the child window against `parent`. Caller positions it via
    // MoveWindow, same as any other child control.
    HWND Create(HWND parent);

    // Replaces the row list with `order`, looking up each window's
    // current title/icon fresh.
    void SetSelected(const std::vector<HWND>& order);

    // Fired after a drag-reorder completes, with the full new order (of
    // every currently-selected hwnd) -- the caller's own selectedOrder_
    // becomes exactly this.
    void SetOnReordered(std::function<void(std::vector<HWND>)> callback) { onReordered_ = std::move(callback); }

    // Fired by a row's Remove button click.
    void SetOnRemoveRequested(std::function<void(HWND)> callback) { onRemoveRequested_ = std::move(callback); }

    // Fired when a row's body (anywhere but Remove/the grip) is clicked,
    // with that row's hwnd -- StackPickerWindow owns what "selected"
    // actually means across both lists; this just reports the gesture.
    void SetOnRowSelected(std::function<void(HWND)> callback) { onRowSelected_ = std::move(callback); }

    // Updates which row (if any) renders as selected, matched by hwnd
    // identity against the current rows_ -- see
    // StackPickerListWindow::SetSelectedHwnd's identical contract (pure
    // selection repaint, no scrollOffset_/hoveredIndex_/rows_ change).
    void SetSelectedHwnd(std::optional<HWND> hwnd);

    // Whether a drag-reorder is currently in progress -- lets
    // StackPickerWindow's live candidate-refresh timer skip a rebuild
    // that would otherwise yank rows_ out from under the user's mouse
    // mid-drag.
    bool IsDragging() const { return dragging_; }

    HWND WindowHandle() const { return window_; }

private:
    struct RowLayout {
        std::vector<RECT> rowRects;  // natural (unshifted-by-scroll) coordinates
        int contentHeight = 0;
    };

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& clientRect) const;
    RowLayout ComputeLayout(UINT dpi) const;
    void UpdateScrollInfo();
    void SetHoveredIndex(std::optional<size_t> index);
    void BeginDrag(size_t index);
    void UpdateDrag(POINT clientPt);
    void EndDrag();
    // Shows/repositions/hides the "Remove from stack"/"Drag to reorder"
    // tooltip to match whichever control (if any) is under the cursor on
    // the currently-hovered row -- see
    // StackPickerListWindow::UpdateTooltip for the identical TTF_TRACK-
    // based technique this mirrors, extended to two hover targets.
    void UpdateTooltip();
    // Scrolls just far enough to bring row `index` fully into view -- see
    // StackPickerListWindow::EnsureRowVisible's identical contract.
    // One viewport's worth of rows, for PageUp/PageDown. Never 0.
    size_t RowsPerPage() const;
    void EnsureRowVisible(size_t index);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND tooltipWindow_ = nullptr;
    // Whether this panel currently owns keyboard focus -- drives its own
    // accent border (see Paint), nothing else.
    bool hasFocus_ = false;
    std::vector<StackPickerSelectedRow> rows_;
    // Which row (if any) renders as selected -- pushed down from
    // StackPickerWindow's shared selectedWindow_ via SetSelectedHwnd,
    // same "single source of truth lives one level up" shape
    // StackPickerListWindow's own selectedIndex_ uses. Independent of
    // hoveredIndex_ -- either alone is enough to reveal a row's
    // Remove button and drag grip (see this class's own header comment).
    std::optional<size_t> selectedIndex_;
    std::optional<size_t> hoveredIndex_;
    bool dragging_ = false;
    size_t dragIndex_ = 0;
    int scrollOffset_ = 0;
    std::function<void(std::vector<HWND>)> onReordered_;
    std::function<void(HWND)> onRemoveRequested_;
    std::function<void(HWND)> onRowSelected_;
};

}  // namespace polish
