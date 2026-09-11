#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace polish {

// One row: a candidate window, whether it's currently a group member
// (checked_), its title, and its icon. Icon is captured once (in
// SetWindows) and borrowed -- same "never destroyed here" contract as
// AltTabListRow::icon / GroupChromeWindow::SetMemberIcons.
struct GroupPickerRow {
    HWND hwnd = nullptr;
    std::wstring title;
    HICON icon = nullptr;  // borrowed, may be nullptr (falls back to text-only)
    bool checked = false;
};

// The single Windows-11-style checklist that replaced GroupPickerWindow's
// old two-list ("Open windows" / "Group") + Add/Remove/Move Up/Move Down
// model: one list of every candidate window, each row showing a checkbox
// (group membership), the window's own icon, its title, and -- on a
// checked row only -- a drag handle for reordering.
//
// Invariant `rows_` always maintains: every checked row is a contiguous
// prefix, in committed group order; every unchecked row follows, in
// stable candidate order. CheckedWindows() just reads that prefix -- the
// same "vector order IS the committed order" contract
// GroupPickerWindow's old groupWindows_ vector had.
//
// Interaction model (deliberately unambiguous -- see class's own history
// in PLAN.md for why): dragging reorders only within the checked prefix
// (the grip is only ever drawn/hit-testable on a checked row); clicking
// anywhere else on a row toggles its checked state. Checking moves a row
// to the end of the checked prefix; unchecking moves it to the end of
// the whole list -- mirroring the old Add/Remove buttons' own
// append-to-end behavior exactly.
//
// Modeled on AltTabListWindow's ComputeLayout/Paint/hit-test/scroll
// shape (the closest existing precedent for a hand-rolled, DPI-aware,
// scrollable icon+text list in this codebase), but a plain WS_CHILD --
// opaque, normal z-order, created fresh in
// GroupPickerWindow::CreateControls and destroyed with it -- not
// AltTabListWindow's WS_POPUP/WS_EX_LAYERED/topmost/created-once shape,
// which solves a different (translucent overlay) problem.
//
// Known, accepted gap (v1): mouse-only, like AltTabListWindow itself --
// SysListView32's free Tab/arrow-key/Space keyboard navigation is not
// reproduced here yet.
class GroupPickerListWindow {
public:
    explicit GroupPickerListWindow(HINSTANCE instance);
    ~GroupPickerListWindow();

    GroupPickerListWindow(const GroupPickerListWindow&) = delete;
    GroupPickerListWindow& operator=(const GroupPickerListWindow&) = delete;

    // Creates the child window against `parent`. Caller positions it via
    // MoveWindow, same as any other child control.
    HWND Create(HWND parent);

    // Replaces the row list: `initialChecked` (kept even if not present
    // in `candidates` -- preserves the "never silently drop a member"
    // contract an edit-time-filtered, e.g. minimized, member needs)
    // followed by whatever of `candidates` isn't already checked. Fires
    // onChanged_ once.
    void SetWindows(const std::vector<HWND>& candidates, const std::vector<HWND>& initialChecked);

    // The checked prefix of rows_, in order -- the final committed group
    // membership.
    std::vector<HWND> CheckedWindows() const;

    // Fired whenever CheckedWindows()'s result could have changed (a
    // check/uncheck or a drag-reorder) -- the parent uses this to keep
    // e.g. its Create-button enabled state in sync.
    void SetOnChanged(std::function<void()> callback) { onChanged_ = std::move(callback); }

    HWND WindowHandle() const { return window_; }

private:
    struct RowLayout {
        std::vector<RECT> rowRects;  // natural (unshifted-by-scroll) coordinates
        int contentHeight = 0;
    };

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& clientRect) const;
    // Single source of truth for row rects -- Paint, WM_LBUTTONDOWN/
    // WM_MOUSEMOVE hit-testing, and scroll-range computation all derive
    // from this one pass.
    RowLayout ComputeLayout(UINT dpi) const;
    void UpdateScrollInfo();
    size_t CheckedCount() const;
    void SetHoveredIndex(std::optional<size_t> index);
    void ToggleChecked(size_t index);
    void BeginDrag(size_t index);
    void UpdateDrag(POINT clientPt);
    void EndDrag();
    void NotifyChanged();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::vector<GroupPickerRow> rows_;
    std::optional<size_t> hoveredIndex_;
    bool dragging_ = false;
    size_t dragIndex_ = 0;
    // Vertical pixel offset, only nonzero once there are more rows than
    // fit in the child's current height -- same "natural unshifted
    // ComputeLayout rects, SetViewportOrgEx at paint time, hit-testers
    // shift the point instead" pattern AltTabListWindow uses.
    int scrollOffset_ = 0;
    std::function<void()> onChanged_;
};

}  // namespace polish
