#include "hook/StackHotkeyDialog.h"

#include <iterator>

#include "util/DialogKeyboard.h"
#include "util/UiFont.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishStackHotkeyDialog";
constexpr int kCtrlCheckId = 2001;
constexpr int kAltCheckId = 2002;
constexpr int kShiftCheckId = 2003;
constexpr int kWinCheckId = 2004;
constexpr int kKeyEditId = 2005;
constexpr int kSaveButtonId = 2006;
constexpr int kCancelButtonId = 2007;

// Logical (96 DPI) layout constants.
constexpr int kWindowWidth = 320;
constexpr int kWindowHeight = 230;
constexpr int kMargin = 12;
constexpr int kCheckHeight = 24;
constexpr int kEditWidth = 40;
constexpr int kEditHeight = 24;
constexpr int kButtonHeight = 28;
constexpr int kButtonWidth = 90;
constexpr int kLabelHeight = 20;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

}  // namespace

StackHotkeyDialog::StackHotkeyDialog(HINSTANCE instance) : instance_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
}

StackHotkeyDialog::~StackHotkeyDialog() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK StackHotkeyDialog::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    StackHotkeyDialog* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<StackHotkeyDialog*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StackHotkeyDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT StackHotkeyDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateControls(hwnd);
            return 0;

        case WM_SIZE:
            LayoutControls();
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (LOWORD(wParam) == kSaveButtonId) {
                    Commit();
                } else if (LOWORD(wParam) == kCancelButtonId) {
                    result_.reset();
                    done_ = true;
                }
            } else if (LOWORD(wParam) == kKeyEditId && HIWORD(wParam) == EN_CHANGE) {
                // Keep only the most recently typed character -- see
                // CreateControls' comment on why the field allows more
                // than 1 character rather than limiting to exactly 1.
                wchar_t text[8] = L"";
                GetWindowTextW(keyEdit_, text, 8);
                const size_t length = wcslen(text);
                if (length > 1) {
                    const wchar_t lastChar[2] = {text[length - 1], L'\0'};
                    SetWindowTextW(keyEdit_, lastChar);
                    SendMessageW(keyEdit_, EM_SETSEL, 1, 1);  // caret after the single remaining character
                }
            }
            return 0;

        case WM_CLOSE:
            result_.reset();
            done_ = true;
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void StackHotkeyDialog::CycleFocus(bool backward) {
    // Reading order: the four modifier checkboxes, the key field, then
    // the two buttons.
    const HWND stops[] = {ctrlCheck_, altCheck_, shiftCheck_, winCheck_, keyEdit_, saveButton_, cancelButton_};
    polish::CycleFocus(stops, std::size(stops), backward);
}

void StackHotkeyDialog::CreateControls(HWND hwnd) {
    // MakeUiFont, not GetStockObject(DEFAULT_GUI_FONT) -- the same
    // fixed, pre-DPI-awareness font that left the stack strip's tab
    // labels tiny at high DPI (see util/UiFont.h). Owned now, so
    // ShowModal's cleanup deletes it; the stock font it replaced must
    // never be deleted.
    if (dialogFont_ != nullptr) {
        DeleteObject(dialogFont_);
    }
    dialogFont_ = MakeUiFont(GetDpiForWindow(hwnd));
    HFONT dialogFont = dialogFont_;

    ctrlCheck_ = CreateWindowExW(0, L"BUTTON", L"Ctrl", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlCheckId)), instance_, nullptr);
    altCheck_ = CreateWindowExW(0, L"BUTTON", L"Alt", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAltCheckId)), instance_, nullptr);
    shiftCheck_ = CreateWindowExW(0, L"BUTTON", L"Shift", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShiftCheckId)), instance_, nullptr);
    winCheck_ = CreateWindowExW(0, L"BUTTON", L"Win", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWinCheckId)), instance_, nullptr);

    SendMessageW(ctrlCheck_, BM_SETCHECK, (initialChoice_.modifiers & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(altCheck_, BM_SETCHECK, (initialChoice_.modifiers & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(shiftCheck_, BM_SETCHECK, (initialChoice_.modifiers & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(winCheck_, BM_SETCHECK, (initialChoice_.modifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);

    keyEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_CENTER | ES_UPPERCASE,
                                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kKeyEditId)),
                                instance_, nullptr);
    // Limit of 1 would mean a user can never actually type a
    // *replacement* key -- the field starts pre-filled with the
    // current key, already at a 1-char limit, so there's no room left
    // to insert a new character alongside it (confirmed: produces
    // EN_MAXTEXT, the keystroke is silently rejected). Instead, allow a
    // little room and auto-trim to just the most recently typed
    // character in WM_COMMAND's EN_CHANGE handling below, which gives
    // the same "whatever you just typed replaces what was there" feel
    // without requiring the user to select/clear the field first.
    SendMessageW(keyEdit_, EM_SETLIMITTEXT, 4, 0);
    if (initialChoice_.virtualKey != 0) {
        const wchar_t keyChar[2] = {static_cast<wchar_t>(initialChoice_.virtualKey), L'\0'};
        SetWindowTextW(keyEdit_, keyChar);
    }

    saveButton_ = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButtonId)), instance_, nullptr);
    cancelButton_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), instance_,
                                     nullptr);

    for (HWND control : {ctrlCheck_, altCheck_, shiftCheck_, winCheck_, keyEdit_, saveButton_, cancelButton_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    }

    LayoutControls();
}

void StackHotkeyDialog::LayoutControls() {
    if (ctrlCheck_ == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_ != nullptr ? window_ : ctrlCheck_);
    RECT client{};
    GetClientRect(GetParent(ctrlCheck_), &client);

    const int margin = Scale(kMargin, dpi);
    const int checkHeight = Scale(kCheckHeight, dpi);
    const int labelHeight = Scale(kLabelHeight, dpi);
    const int editWidth = Scale(kEditWidth, dpi);
    const int editHeight = Scale(kEditHeight, dpi);
    const int buttonHeight = Scale(kButtonHeight, dpi);
    const int buttonWidth = Scale(kButtonWidth, dpi);
    const int checkWidth = (client.right - 2 * margin) / 4;

    int y = margin + labelHeight;
    MoveWindow(ctrlCheck_, margin, y, checkWidth, checkHeight, TRUE);
    MoveWindow(altCheck_, margin + checkWidth, y, checkWidth, checkHeight, TRUE);
    MoveWindow(shiftCheck_, margin + 2 * checkWidth, y, checkWidth, checkHeight, TRUE);
    MoveWindow(winCheck_, margin + 3 * checkWidth, y, checkWidth, checkHeight, TRUE);

    y += checkHeight + margin + labelHeight;
    MoveWindow(keyEdit_, margin, y, editWidth, editHeight, TRUE);

    const int buttonsTop = client.bottom - margin - buttonHeight;
    MoveWindow(cancelButton_, client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(saveButton_, client.right - 2 * margin - 2 * buttonWidth, buttonsTop, buttonWidth, buttonHeight,
               TRUE);
}

void StackHotkeyDialog::Commit() {
    UINT modifiers = 0;
    if (SendMessageW(ctrlCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) modifiers |= MOD_CONTROL;
    if (SendMessageW(altCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) modifiers |= MOD_ALT;
    if (SendMessageW(shiftCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) modifiers |= MOD_SHIFT;
    if (SendMessageW(winCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) modifiers |= MOD_WIN;

    wchar_t keyText[8] = L"";
    GetWindowTextW(keyEdit_, keyText, 8);
    const wchar_t keyChar = keyText[0];

    const bool hasModifier = modifiers != 0;
    const bool hasValidKey = (keyChar >= L'A' && keyChar <= L'Z') || (keyChar >= L'0' && keyChar <= L'9');
    if (!hasModifier || !hasValidKey) {
        MessageBoxW(window_, L"Pick at least one modifier (Ctrl/Alt/Shift/Win) and a single letter or digit key.",
                    L"Invalid shortcut", MB_OK | MB_ICONWARNING);
        return;
    }

    result_ = HotkeyChoice{modifiers, static_cast<UINT>(keyChar)};
    done_ = true;
}

std::optional<HotkeyChoice> StackHotkeyDialog::ShowModal(HWND owner, const HotkeyChoice& current) {
    initialChoice_ = current;

    const UINT dpi = GetDpiForSystem();
    const int width = Scale(kWindowWidth, dpi);
    const int height = Scale(kWindowHeight, dpi);

    RECT monitorRect{};
    HMONITOR monitor = (owner != nullptr && IsWindow(owner)) ? MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY)
                                                               : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorRect = monitorInfo.rcWork;
    }
    const int x = monitorRect.left + ((monitorRect.right - monitorRect.left) - width) / 2;
    const int y = monitorRect.top + ((monitorRect.bottom - monitorRect.top) - height) / 2;

    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName, L"Change Stack Hotkey",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr, instance_,
                               this);
    if (window_ == nullptr) {
        return std::nullopt;
    }

    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    // Start on the first modifier checkbox -- the first of CycleFocus's
    // tab stops. Without this the dialog opened with focus nowhere at
    // all, which (combined with having had no Tab handling) meant it
    // could not be operated from the keyboard *whatsoever* until
    // something was clicked first.
    SetFocus(ctrlCheck_);

    done_ = false;
    result_.reset();
    MSG msg;
    while (!done_) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            break;
        }
        if (IsDialogKeyDown(msg, window_, VK_ESCAPE)) {
            result_.reset();
            done_ = true;
            break;
        }
        if (IsDialogKeyDown(msg, window_, VK_RETURN)) {
            Commit();
            if (done_) {
                break;
            }
            continue;  // Commit() showed a validation error and left the dialog open
        }
        // Swallowed before TranslateMessage so the key field never sees
        // a literal tab character -- same interception point and
        // reasoning as StackPickerWindow's own pump. This dialog is not
        // a real Win32 dialog either (custom class, own modal loop), so
        // BS_DEFPUSHBUTTON on Save and the checkboxes' own WS_TABSTOP-ish
        // expectations get no help from a dialog manager: every stop has
        // to be walked by hand.
        if (IsDialogKeyDown(msg, window_, VK_TAB)) {
            CycleFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(window_);
    window_ = nullptr;
    if (dialogFont_ != nullptr) {
        // After DestroyWindow, so no control still has it selected.
        DeleteObject(dialogFont_);
        dialogFont_ = nullptr;
    }
    ctrlCheck_ = nullptr;
    altCheck_ = nullptr;
    shiftCheck_ = nullptr;
    winCheck_ = nullptr;
    keyEdit_ = nullptr;
    saveButton_ = nullptr;
    cancelButton_ = nullptr;
    return result_;
}

}  // namespace polish
