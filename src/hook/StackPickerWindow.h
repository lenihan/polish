#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "hook/StackPickerListWindow.h"
#include "hook/StackPickerSelectedListWindow.h"

namespace polish {

struct StackPickerResult {
    std::vector<HWND> windows;  // final Stack-list membership, in order
    std::wstring name;
};

// The unified "New Stack"/"Manage windows..." dialog: two side-by-side,
// mutually-exclusive lists -- "Available windows" on the left (every
// *not yet selected* candidate, each row with an Add button pointing
// right -- see StackPickerListWindow) and "Stack" on the right (current
// membership, in order, each row with a Remove button pointing left
// back toward Available plus a drag handle for reordering -- see
// StackPickerSelectedListWindow). A window lives in exactly one of the
// two lists at a time; the Add/Remove arrows point toward whichever
// side a window would move to, the same spatial convention this
// dialog's original (pre-Windows-11-redesign) two-list layout used.
// Order feeds directly into the strip's tab order.
//
// This shape went through several iterations before landing here: a
// single flat checklist that sorted checked rows to the top (confirmed,
// human-reported: felt like the list "jumped" out from under a user
// mid-browse); a two-panel *stacked* version using checkboxes in
// Available and a drag-handle-plus-click-to-remove Selected panel
// (confirmed, human-reported: an imprecise drag attempt would silently
// register as an accidental removal instead, and the Selected panel's
// height visibly resizing the whole dialog on every selection change
// read as jank); a version replacing that drag handle with explicit
// Move Up/Move Down buttons (confirmed, human-reported: clunkier than a
// drag handle for reordering). This version keeps an explicit Remove
// button (so a near-missed drag still can't register as a removal --
// the row body itself does nothing) while bringing the drag handle back
// for reordering, and returns to a side-by-side layout instead of
// stacking one panel's dynamic height above the other. selectedOrder_
// (below) is this class's own single source of truth for stack
// membership/order; both child lists are pure renderers driven from it
// (available_ shown ones are allCandidates_ minus selectedOrder_) and
// report user gestures back up via callbacks, never holding their own
// independent copy of "what's selected."
//
// Not a real Win32 modal dialog (no .rc dialog template, no DialogBoxW)
// -- a plain WS_POPUP top-level window built the same from-scratch way
// as this app's other custom windows, with its own nested message loop
// run from ShowModal for the duration it's open.
class StackPickerWindow {
public:
    explicit StackPickerWindow(HINSTANCE instance);
    ~StackPickerWindow();

    StackPickerWindow(const StackPickerWindow&) = delete;
    StackPickerWindow& operator=(const StackPickerWindow&) = delete;

    // Shows the dialog, centered against `owner` (or the primary
    // monitor if owner isn't valid), and blocks -- pumping messages via
    // its own nested loop -- until the user confirms or cancels/closes
    // it. Returns the final Stack-list membership (in its final order)
    // and name on confirm, or std::nullopt on cancel.
    //
    // `initialSelection` pre-selects those rows (used for editing an
    // existing stack's membership -- an empty list is exactly
    // creation); existing members are always kept selected even if
    // they'd normally be filtered out of "candidates" (e.g. currently
    // minimized) -- editing should never silently drop a member just
    // because of a transient state at edit time. `initialName`
    // pre-fills the name field. `editing` only affects the window
    // title/confirm-button wording.
    std::optional<StackPickerResult> ShowModal(HWND owner, const std::vector<HWND>& initialSelection = {},
                                                const std::wstring& initialName = L"New Stack",
                                                bool editing = false);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HWND hwnd);
    void LayoutControls();
    // (Re)creates dialogFont_ at the given DPI and re-pushes WM_SETFONT
    // to every control that uses it, freeing the previous HFONT only
    // after the new one is in place (freeing first would leave those
    // controls briefly holding a deleted GDI object). Called once from
    // CreateControls and again on every WM_DPICHANGED.
    void ApplyDialogFont(UINT dpi);
    // Moves keyboard focus to the next (or previous) of this dialog's five
    // tab stops, wrapping: Name, Open windows, Stack, Create, Cancel. Done
    // by hand because this deliberately isn't a real Win32 dialog (see the
    // class comment) -- there's no IsDialogMessage/WS_TABSTOP machinery
    // here to defer to, and two of the five stops are custom-painted child
    // windows that dialog navigation wouldn't know how to drive anyway.
    void CycleFocus(bool backward);
    void PopulateLists();
    // allCandidates_ minus whatever's currently in selectedOrder_, in
    // allCandidates_'s own stable order -- exactly the row set the
    // "Open windows" panel shows (the two lists are mutually exclusive,
    // see class comment). Its own function because the add/remove
    // callbacks need to see this list both before and after they mutate
    // selectedOrder_, to work out which row should inherit the moved
    // one's slot.
    std::vector<HWND> AvailableWindows() const;
    // Pushes selectedOrder_'s current state down to both child lists
    // (available_'s row set, selected_'s row list) -- the one place
    // that keeps them in sync with each other and with selectedOrder_
    // itself, called after every mutation of it. Also the one place
    // selectedWindow_ is validated (falls back to a sensible default if
    // it's unset or no longer refers to a live row) and pushed down to
    // both lists via SetSelectedHwnd.
    void RefreshLists();
    // Re-enumerates open windows (the same filter PopulateLists' own
    // EnumPickerCandidatesProc uses) on a WM_TIMER tick and reconciles
    // allCandidates_/selectedOrder_ against it, order-preserving the same
    // way src/main.cpp's UpdateAltTabCandidatesPreservingOrder does for
    // Alt+Tab -- survivors keep their position, closed windows drop out,
    // new ones append at the end. A no-op (skips RefreshLists entirely)
    // when nothing actually changed, since SetWindows/SetSelected reset
    // scroll/hover state and re-fetch every row's icon -- see this
    // method's own .cpp comment for why that matters here specifically.
    void RefreshCandidates();
    // Dumps the current candidate list to the debug log as
    // hwnd:"title"[class], the same shape main.cpp's Alt+Tab session dump
    // uses. Exists because "window X isn't in the list" was otherwise
    // undiagnosable from this side: several independent filters can drop
    // a window (IsCandidateWindowShape's visible/owner/toolwindow/caption/
    // title-length/cloaked checks, plus this dialog's own fail-closed
    // IsElevatedWindow), and a window that's already a member of another
    // stack is WS_CHILD and so never reaches EnumWindows at all. Having
    // the accepted set in the log turns "it's missing" into "it's
    // missing *and* here's everything that wasn't".
    void LogCandidates(const wchar_t* reason) const;
    void UpdateButtonStates();
    void Commit();
    void ApplyDarkMode();
    void DrawOwnerButton(const DRAWITEMSTRUCT& item);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND nameLabel_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND selectedLabel_ = nullptr;
    HWND availableLabel_ = nullptr;
    HWND createButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    StackPickerSelectedListWindow selected_;
    StackPickerListWindow available_;

    // The single source of truth for stack membership/order (see class
    // comment) -- both child panels are refreshed from this, never the
    // other way around. allCandidates_ is the fixed, never-reordered
    // universe available_ browses; built once per ShowModal in
    // PopulateLists.
    std::vector<HWND> selectedOrder_;
    std::vector<HWND> allCandidates_;

    // The single cross-list selection -- since a window is only ever in
    // one of the two lists at a time (see class comment), one shared
    // HWND is sufficient to guarantee at most one row reads as
    // "selected" across the whole dialog at once. Both child lists are
    // pure renderers of this, updated via their own SetSelectedHwnd;
    // never decide their own default independently. Validated and
    // pushed down in RefreshLists (see its own comment).
    std::optional<HWND> selectedWindow_;

    std::vector<HWND> initialSelection_;
    std::wstring initialName_;
    bool editing_ = false;
    std::optional<StackPickerResult> result_;
    bool done_ = false;

    // The rounded-rect "card" LayoutControls positions nameEdit_ within
    // (client coords) -- cached from LayoutControls so WM_ERASEBKGND can
    // draw the same rect behind the control without recomputing the
    // layout math a second place it could drift out of sync with.
    RECT nameFieldRect_{};

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

    // The dialog's own DPI-aware text font (NONCLIENTMETRICS'
    // lfMessageFont, the same source both list panels already use) --
    // GetStockObject(DEFAULT_GUI_FONT) doesn't scale with DPI, unlike
    // every control's own box, which left text tiny inside correctly-
    // sized controls at high DPI (confirmed, human-reported). Rebuilt by
    // ApplyDialogFont on creation and on every WM_DPICHANGED; freed at
    // the end of ShowModal, same lifecycle as backgroundBrush_/
    // editBackgroundBrush_ below (this object is reused across multiple
    // ShowModal calls).
    HFONT dialogFont_ = nullptr;

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
