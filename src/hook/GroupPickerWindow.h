#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "hook/GroupPickerListWindow.h"

namespace polish {

struct GroupPickerResult {
    std::vector<HWND> windows;  // final Group-list membership, in order
    std::wstring name;
};

// The unified "New Group"/"Manage windows..." dialog: a single
// Windows-11-style checklist (see GroupPickerListWindow) listing every
// candidate window, each row a checkbox (group membership) + icon +
// title, reorderable by dragging a checked row's grip handle -- order
// feeds directly into GroupChromeWindow's tab order and GroupManager's
// tile fill order.
//
// Not a real Win32 modal dialog (no .rc dialog template, no DialogBoxW)
// -- a plain WS_POPUP top-level window built the same from-scratch way
// as this app's other custom windows, with its own nested message loop
// run from ShowModal for the duration it's open.
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
    // `initialSelection` pre-checks those rows (used for editing an
    // existing group's membership -- an empty list is exactly
    // creation); existing members are always kept in the list even if
    // they'd normally be filtered out of "candidates" (e.g. currently
    // minimized) -- editing should never silently drop a member just
    // because of a transient state at edit time. `initialName`
    // pre-fills the name field. `editing` only affects the window
    // title/confirm-button wording.
    std::optional<GroupPickerResult> ShowModal(HWND owner, const std::vector<HWND>& initialSelection = {},
                                                const std::wstring& initialName = L"New Group",
                                                bool editing = false);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HWND hwnd);
    void LayoutControls();
    void PopulateList();
    void UpdateButtonStates();
    void Commit();
    void ApplyDarkMode();
    void DrawOwnerButton(const DRAWITEMSTRUCT& item);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND nameLabel_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND createButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    GroupPickerListWindow list_;

    std::vector<HWND> initialSelection_;
    std::wstring initialName_;
    bool editing_ = false;
    std::optional<GroupPickerResult> result_;
    bool done_ = false;

    // Backs WM_CTLCOLORSTATIC/WM_CTLCOLOREDIT/WM_ERASEBKGND -- created
    // once per ShowModal (matching window_'s own lifecycle) in
    // ApplyDarkMode, freed alongside the rest of window_'s state when
    // it's destroyed. A brush returned from those messages must stay
    // valid for the system to paint with; recreating one per message
    // without ever freeing it would leak a GDI object per repaint.
    // editBackgroundBrush_ is a slightly lighter shade than
    // backgroundBrush_ so the name field still reads as an editable
    // control rather than blending completely into the dialog.
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBackgroundBrush_ = nullptr;

    // Set in ApplyDarkMode from the actual DwmSetWindowAttribute result
    // for DWMWA_SYSTEMBACKDROP_TYPE -- currently unused by any painting
    // decision (see WM_ERASEBKGND's own comment for why: the call
    // reports success but no translucent backdrop visibly renders on
    // this host, so this dialog always paints an opaque background
    // regardless of this flag). Kept for future diagnosis/use rather
    // than discarding the one signal that confirms the API call itself
    // is/isn't succeeding.
    bool micaEnabled_ = false;
};

}  // namespace polish
