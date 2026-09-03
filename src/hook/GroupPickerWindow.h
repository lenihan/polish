#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace polish {

struct GroupPickerResult {
    std::vector<HWND> windows;  // final Group-list membership, in order
    std::wstring name;
};

// The unified "New Group"/"Manage windows..." dialog: two side-by-side
// lists -- "Active windows" (open candidates not yet in the group) and
// "Group" (current membership, in order) -- with Add/Remove (or a
// double-click) moving a selection between them, and the Group list
// reorderable both by drag (order feeds directly into
// GroupChromeWindow's tab order and GroupManager's tile fill order)
// and via explicit Move Up/Move Down buttons -- drag alone isn't
// discoverable, so the buttons are the primary, always-visible path.
// No Tab/Tile choice here anymore -- that's moving to the chrome's own
// title-bar controls (see PLAN.md's milestone plan, M4) -- and no
// checkbox model either; membership is purely "which list a window is
// currently in."
//
// Not a real Win32 modal dialog (no .rc dialog template, no DialogBoxW)
// -- a plain WS_POPUP top-level window built the same from-scratch way
// as this app's other custom windows, with its own nested message loop
// run from ShowModal for the duration it's open. Requires Common
// Controls v6 (already declared in app.manifest) for the list views.
class GroupPickerWindow {
public:
    explicit GroupPickerWindow(HINSTANCE instance);
    ~GroupPickerWindow();

    GroupPickerWindow(const GroupPickerWindow&) = delete;
    GroupPickerWindow& operator=(const GroupPickerWindow&) = delete;

    // Shows the dialog, centered against `owner` (or the primary
    // monitor if owner isn't valid), and blocks -- pumping messages via
    // its own nested loop -- until the user confirms or cancels/closes
    // it. Returns the final Group-list membership (in its final order)
    // and name on confirm, or std::nullopt on cancel.
    //
    // `initialSelection` pre-populates the Group list (used for editing
    // an existing group's membership -- an empty list is exactly
    // creation); existing members are always kept in the Group list
    // even if they'd normally be filtered out of "Active windows" (e.g.
    // currently minimized) -- editing should never silently drop a
    // member just because of a transient state at edit time.
    // `initialName` pre-fills the name field. `editing` only affects
    // the window title/confirm-button wording.
    std::optional<GroupPickerResult> ShowModal(HWND owner, const std::vector<HWND>& initialSelection = {},
                                                const std::wstring& initialName = L"New Group",
                                                bool editing = false);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleNotify(NMHDR* header);

    void CreateControls(HWND hwnd);
    void LayoutControls();
    void PopulateLists();
    void RefreshListView(HWND listView, const std::vector<HWND>& windows);
    void MoveSelection(HWND fromListView, std::vector<HWND>& from, std::vector<HWND>& to);
    void MoveSingle(std::vector<HWND>& from, std::vector<HWND>& to, size_t index);
    void MoveSelectedInGroupList(int direction);
    void BeginDrag(int itemIndex);
    void UpdateDrag(POINT screenPt);
    void EndDrag();
    void Commit();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND activeListLabel_ = nullptr;
    HWND groupListLabel_ = nullptr;
    HWND activeListView_ = nullptr;
    HWND groupListView_ = nullptr;
    HWND addButton_ = nullptr;
    HWND removeButton_ = nullptr;
    HWND moveUpButton_ = nullptr;
    HWND moveDownButton_ = nullptr;
    HWND nameLabel_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND createButton_ = nullptr;
    HWND cancelButton_ = nullptr;

    std::vector<HWND> activeWindows_;  // parallel to activeListView_'s rows
    std::vector<HWND> groupWindows_;   // parallel to groupListView_'s rows, in order

    std::vector<HWND> initialSelection_;
    std::wstring initialName_;
    bool editing_ = false;
    std::optional<GroupPickerResult> result_;
    bool done_ = false;

    // Drag-to-reorder state, groupListView_ only -- reorders live as
    // the dragged row crosses another (same pattern as
    // GroupChromeWindow's own tab drag-reorder), not a separate
    // insert-mark indicator.
    bool dragging_ = false;
    int dragItemIndex_ = -1;
};

}  // namespace polish
