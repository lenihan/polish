#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "windowtracking/GroupState.h"

namespace polish {

struct GroupPickerResult {
    std::vector<HWND> windows;
    GroupMode mode = GroupMode::Tab;
};

// The "New Group" creation UI: lists currently open candidate windows
// (same filter as Alt+Tab, see WindowFilters.h, minus elevated windows
// -- see IsElevatedWindow's comment for why those are excluded rather
// than offered) with checkboxes, plus a Tab/Tile mode choice (v1 has no
// runtime mode switching -- M7+ -- so the choice is made once, here, at
// creation time), and returns the confirmed selection.
//
// Not a real Win32 modal dialog (no .rc dialog template, no DialogBoxW)
// -- a plain WS_POPUP top-level window built the same from-scratch way
// as this app's other custom windows (AltTabDimOverlay,
// AltTabHighlightBorder), with its own nested message loop run from
// ShowModal for the duration it's open. Requires Common Controls v6
// (already declared in app.manifest) for the checkbox-enabled list view.
class GroupPickerWindow {
public:
    explicit GroupPickerWindow(HINSTANCE instance);
    ~GroupPickerWindow();

    GroupPickerWindow(const GroupPickerWindow&) = delete;
    GroupPickerWindow& operator=(const GroupPickerWindow&) = delete;

    // Shows the picker, centered against `owner` (or the primary monitor
    // if owner isn't valid), and blocks -- pumping messages via its own
    // nested loop -- until the user confirms or cancels/closes it.
    // Returns the checked windows (possibly empty) and chosen mode on
    // confirm, or std::nullopt on cancel.
    //
    // `initialSelection`/`initialMode` pre-check the corresponding rows
    // and pre-select the corresponding radio button -- used for editing
    // an existing group's membership/mode, not just creating a new one
    // (an empty `initialSelection` and the default `initialMode` is
    // exactly creation). Existing members are always kept in the list
    // even if they'd normally be filtered out (e.g. currently
    // minimized) -- editing should never silently drop a member just
    // because of a transient state at edit time. `editing` only affects
    // the window title/confirm-button wording.
    std::optional<GroupPickerResult> ShowModal(HWND owner, const std::vector<HWND>& initialSelection = {},
                                                GroupMode initialMode = GroupMode::Tab, bool editing = false);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HWND hwnd);
    void LayoutControls();
    void PopulateList();
    void Commit();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND listView_ = nullptr;
    HWND tabModeRadio_ = nullptr;
    HWND tileModeRadio_ = nullptr;
    HWND createButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    std::vector<HWND> candidates_;  // parallel to the list view's rows
    std::vector<HWND> initialSelection_;
    GroupMode initialMode_ = GroupMode::Tab;
    bool editing_ = false;
    std::optional<GroupPickerResult> result_;
    bool done_ = false;
};

}  // namespace polish
