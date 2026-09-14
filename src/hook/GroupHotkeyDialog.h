#pragma once

#include <windows.h>

#include <optional>

namespace polish {

struct HotkeyChoice {
    UINT modifiers = 0;      // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, OR'd
    UINT virtualKey = 0;     // a single VK code, e.g. 'G'
};

// The "Change Group Hotkey..." UI: four modifier checkboxes (Ctrl/Alt/
// Shift/Win) plus a one-character edit box, not a live "press your
// shortcut" capture -- the Win key in particular is unreliable to
// detect via a live key-press (the shell often intercepts it first),
// and RegisterHotKey wants an explicit modifier-flags + VK pair anyway,
// which checkboxes map onto directly.
//
// Purely a collection/validation UI, same division of responsibility
// as GroupPickerWindow: this dialog doesn't itself call RegisterHotKey
// -- the caller owns the actual hotkey id/message window and the
// try-register-or-retry loop (a combination can be syntactically valid
// here but still already claimed by something else on the machine,
// which this dialog has no way to know).
class GroupHotkeyDialog {
public:
    explicit GroupHotkeyDialog(HINSTANCE instance);
    ~GroupHotkeyDialog();

    GroupHotkeyDialog(const GroupHotkeyDialog&) = delete;
    GroupHotkeyDialog& operator=(const GroupHotkeyDialog&) = delete;

    // Shows the dialog pre-filled with `current`, centered against
    // `owner`, and blocks (its own nested message loop) until Save or
    // Cancel. Save requires at least one modifier checked and exactly
    // one A-Z/0-9 character in the key field -- an invalid attempt
    // shows an inline error and keeps the dialog open rather than
    // closing. Returns the chosen modifiers/key on Save, or
    // std::nullopt on Cancel.
    std::optional<HotkeyChoice> ShowModal(HWND owner, const HotkeyChoice& current);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HWND hwnd);
    void LayoutControls();
    void Commit();
    // Moves focus to the next/previous control. By hand, like
    // GroupPickerWindow's own identically-named method (both defer to
    // util/DialogKeyboard) -- this is a custom window class with its own
    // modal pump, not a real Win32 dialog, so there's no dialog manager
    // to walk WS_TABSTOP for us.
    void CycleFocus(bool backward);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    // Owned -- see CreateControls. Deleted in ShowModal's cleanup.
    HFONT dialogFont_ = nullptr;
    HWND ctrlCheck_ = nullptr;
    HWND altCheck_ = nullptr;
    HWND shiftCheck_ = nullptr;
    HWND winCheck_ = nullptr;
    HWND keyEdit_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HotkeyChoice initialChoice_;
    std::optional<HotkeyChoice> result_;
    bool done_ = false;
};

}  // namespace polish
