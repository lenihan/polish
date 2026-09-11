#include "hook/GroupPickerWindow.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <format>

#include "util/DarkMode.h"
#include "util/Logging.h"
#include "windowtracking/WindowFilters.h"

namespace polish {

namespace {

// Not necessarily defined by the SDK's own dwmapi.h (added in the
// Windows 11 SDK) -- same local-fallback-constant pattern
// util/DarkMode.cpp already uses for DWMWA_USE_IMMERSIVE_DARK_MODE.
// Harmless no-ops via DwmSetWindowAttribute on a pre-Win11 host either
// way, so no separate OS-version check is needed.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif
#ifndef DWMWCP_ROUND
constexpr DWORD DWMWCP_ROUND = 2;
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
constexpr DWORD DWMSBT_TRANSIENTWINDOW = 3;  // "Mica Alt" -- for short-lived flyout/dialog surfaces
#endif

constexpr wchar_t kWindowClassName[] = L"PolishGroupPickerWindow";
constexpr int kCreateButtonId = 1003;
constexpr int kCancelButtonId = 1004;

// Logical (96 DPI) layout constants -- scaled by the window's actual DPI
// in LayoutControls/before CreateWindowExW. Width/height re-tuned for a
// single list (no longer needs to fit two lists + two button columns
// side by side) -- narrower, and taller so ~6-7 rows show without
// scrolling on a typical display; margin/button height bumped up for
// the airier spacing Windows 11 dialogs use versus this dialog's
// original, denser layout.
constexpr int kWindowWidth = 460;
constexpr int kWindowHeight = 460;
constexpr int kMargin = 20;
constexpr int kButtonHeight = 32;
constexpr int kButtonWidth = 110;
constexpr int kNameRowHeight = 24;
constexpr int kNameLabelWidth = 50;
constexpr int kButtonCornerRadius = 6;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

COLORREF DarkenColor(COLORREF color, double factor) {
    const int r = static_cast<int>(GetRValue(color) * factor);
    const int g = static_cast<int>(GetGValue(color) * factor);
    const int b = static_cast<int>(GetBValue(color) * factor);
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
}

// A single-line EDIT control does not reliably auto-center its text
// vertically when given a client rect taller than one line -- text just
// sits at the top (confirmed live: reordering WM_SETFONT vs. layout
// didn't change it). Rather than subclass the control to pad its
// non-client area (WM_NCCALCSIZE), the simpler fix used here is to
// give the control only the height text actually needs and center
// *that* short control within its layout row -- no subclassing, no
// WM_NCPAINT interaction with the WS_BORDER border to worry about.
int MeasureLineHeight(HWND hwnd, HFONT font) {
    HDC hdc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd, hdc);
    return metrics.tmHeight + metrics.tmExternalLeading;
}

BOOL CALLBACK EnumPickerCandidatesProc(HWND hwnd, LPARAM lParam) {
    if (IsCandidateWindow(hwnd) && !IsElevatedWindow(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

}  // namespace

GroupPickerWindow::GroupPickerWindow(HINSTANCE instance) : instance_(instance), list_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // No class-level hbrBackground -- WM_ERASEBKGND handles every
        // case itself (an opaque fill when Mica isn't applied, or none
        // at all when it is, see WM_ERASEBKGND's own comment), so
        // DefWindowProcW's own default erase using this brush is never
        // reached. A class brush here would still pre-fill the surface
        // before that decision ever runs, though -- confirmed live as
        // the actual cause of a flat, legacy-light-gray (COLOR_3DFACE
        // isn't dark-mode-aware) margin visible behind Mica instead of
        // the real backdrop.
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
}

GroupPickerWindow::~GroupPickerWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK GroupPickerWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GroupPickerWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<GroupPickerWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GroupPickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT GroupPickerWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateControls(hwnd);
            PopulateList();
            return 0;

        case WM_SIZE:
            LayoutControls();
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case kCreateButtonId:
                        Commit();
                        break;
                    case kCancelButtonId:
                        result_.reset();
                        done_ = true;
                        break;
                    default:
                        break;
                }
            }
            return 0;

        case WM_ERASEBKGND: {
            // Always an opaque themed fill, regardless of micaEnabled_.
            // A transparent erase (to let a real Mica backdrop show
            // through the margins) was tried first -- DwmSetWindowAttribute
            // reports success (see ApplyDarkMode's own log line) but no
            // translucent backdrop actually renders on this host, just a
            // flat, undifferentiated light fill behind everything, which
            // reads as a visual bug (a stray white margin around an
            // otherwise fully dark-themed dialog) rather than a subtle
            // backdrop. A correctly opaque, consistent dialog beats a
            // broken-looking one chasing an effect that isn't visibly
            // paying off in practice -- micaEnabled_ (and the
            // DwmSetWindowAttribute calls themselves) are left in place
            // in case a future Windows/driver combination renders it
            // correctly, but nothing here currently depends on that.
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(hdc, &client,
                      backgroundBrush_ != nullptr ? backgroundBrush_
                                                   : reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
            return 1;
        }

        case WM_DRAWITEM: {
            const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (drawItem != nullptr && drawItem->CtlType == ODT_BUTTON) {
                DrawOwnerButton(*drawItem);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CTLCOLORSTATIC: {
            // The Name label always keeps its own small opaque
            // background (never lets Mica show through here, unlike the
            // bare margins WM_ERASEBKGND leaves transparent) -- Mica
            // samples the real desktop wallpaper behind the window, so
            // it can render lighter than this dialog's own dark text
            // color would have contrast against; confirmed live as a
            // real, human-reported readability problem with a
            // NULL_BRUSH/transparent version of this that was tried
            // first. A few px of non-bleeding label background is a
            // better trade than unreadable text.
            HDC hdcStatic = reinterpret_cast<HDC>(wParam);
            const bool dark = IsDarkModeEnabled();
            SetTextColor(hdcStatic, dark ? RGB(0xE8, 0xE8, 0xE8) : GetSysColor(COLOR_WINDOWTEXT));
            SetBkMode(hdcStatic, TRANSPARENT);
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            const bool dark = IsDarkModeEnabled();
            SetTextColor(hdcEdit, dark ? RGB(0xE8, 0xE8, 0xE8) : GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(hdcEdit, dark ? RGB(0x2B, 0x2B, 0x2B) : GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(editBackgroundBrush_);
        }

        case WM_SETTINGCHANGE:
            // Re-applies everything (title bar, control theming, cached
            // brushes) if the user flips Settings > Personalization >
            // Colors while this dialog is already open, rather than
            // only taking effect on next launch.
            ApplyDarkMode();
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_CLOSE:
            result_.reset();
            done_ = true;
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void GroupPickerWindow::CreateControls(HWND hwnd) {
    // SS_CENTERIMAGE vertically centers a STATIC control's own text
    // within whatever rect it's given -- needed since the label spans
    // the full (taller-than-one-line) name row, to stay visually
    // aligned with the shrunk-and-centered edit box next to it (see
    // MeasureLineHeight's comment).
    nameLabel_ = CreateWindowExW(0, L"STATIC", L"Name:", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0,
                                  0, hwnd, nullptr, instance_, nullptr);
    // WS_BORDER, not WS_EX_CLIENTEDGE -- a flat 1px border reads as
    // Windows 11 (Settings/File Explorer's own text fields); the sunken
    // 3D WS_EX_CLIENTEDGE look is the dated-looking style this pass
    // exists to replace.
    nameEdit_ = CreateWindowExW(0, L"EDIT", initialName_.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, hwnd, nullptr, instance_, nullptr);

    list_.Create(hwnd);
    list_.SetOnChanged([this]() { UpdateButtonStates(); });

    // BS_OWNERDRAW -- confirmed live via a screenshot spike that
    // SetWindowTheme alone (which does correctly theme the edit
    // field's chrome) does not reliably darken BS_PUSHBUTTON on this
    // Windows build; owner-drawing is the only guaranteed-correct path.
    createButton_ = CreateWindowExW(0, L"BUTTON", editing_ ? L"Update Group" : L"Create Group",
                                     WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCreateButtonId)), instance_,
                                     nullptr);
    cancelButton_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)),
                                     instance_, nullptr);

    // Real sizing (LayoutControls) runs before WM_SETFONT, not after --
    // a single-line EDIT control's internal vertical-text-centering
    // offset is computed relative to its size at the time it receives
    // WM_SETFONT, not recomputed on a later resize. Every control here
    // is created at a placeholder 0x0 size, so sending WM_SETFONT before
    // the real MoveWindow left the name field's text pinned to the top
    // instead of centered.
    LayoutControls();

    HFONT dialogFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND control : {nameLabel_, nameEdit_, createButton_, cancelButton_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    }

    ApplyDarkMode();
}

// Applies (or re-applies, on a live WM_SETTINGCHANGE) the current OS
// theme to the whole dialog: the native title bar, cached background
// brushes backing WM_ERASEBKGND/WM_CTLCOLORSTATIC/WM_CTLCOLOREDIT, and
// every themeable child control. The list child reads IsDarkModeEnabled()
// fresh at its own paint time (see GroupPickerListWindow::Paint), so it
// just needs a repaint here, not separate state pushed down to it.
void GroupPickerWindow::ApplyDarkMode() {
    // window_ isn't assigned yet the first time this runs (called from
    // CreateControls, itself called from WM_CREATE, which fires before
    // CreateWindowExW returns) -- nameEdit_'s parent is the same real
    // window handle, already valid at this point.
    HWND dialogHwnd = window_ != nullptr ? window_ : GetParent(nameEdit_);
    const bool dark = IsDarkModeEnabled();
    ApplyDarkTitleBar(dialogHwnd, dark);

    // Rounded corners + Mica backdrop -- idempotent to re-apply on every
    // call, since neither depends on dark/light state. DWMSBT_
    // TRANSIENTWINDOW ("Mica Alt"), not DWMSBT_MAINWINDOW ("Mica") --
    // Microsoft's own guidance reserves the latter for long-lived
    // primary app windows; this dialog is explicitly short-lived (see
    // this class's own header comment).
    DWORD cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(dialogHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
    DWORD backdropType = DWMSBT_TRANSIENTWINDOW;
    const HRESULT backdropResult =
        DwmSetWindowAttribute(dialogHwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    micaEnabled_ = SUCCEEDED(backdropResult);

    if (backgroundBrush_ != nullptr) {
        DeleteObject(backgroundBrush_);
    }
    backgroundBrush_ = CreateSolidBrush(dark ? RGB(0x20, 0x20, 0x20) : GetSysColor(COLOR_3DFACE));
    if (editBackgroundBrush_ != nullptr) {
        DeleteObject(editBackgroundBrush_);
    }
    editBackgroundBrush_ = CreateSolidBrush(dark ? RGB(0x2B, 0x2B, 0x2B) : GetSysColor(COLOR_WINDOW));

    // Buttons are owner-drawn (DrawOwnerButton), not themed via
    // SetWindowTheme -- see their creation comment for why. Only the
    // edit field needs it here, so its scrollbar/selection colors (not
    // its border, which is now a plain WS_BORDER -- see its creation
    // comment) follow dark mode too.
    if (nameEdit_ != nullptr) {
        SetWindowTheme(nameEdit_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }

    // RDW_ALLCHILDREN, not a plain InvalidateRect -- invalidating just
    // the dialog itself doesn't propagate to child windows (each has
    // its own update region), which would leave the owner-drawn
    // buttons/list stuck showing the old theme's colors after a live
    // WM_SETTINGCHANGE until something else happened to repaint them.
    RedrawWindow(dialogHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

// WM_DRAWITEM handler for every button (all BS_OWNERDRAW -- see their
// creation comment for why SetWindowTheme alone wasn't enough). Draws
// a flat fill + 1px border + centered text, using ODS_SELECTED/
// ODS_DISABLED/ODS_FOCUS from the item state for pressed/disabled/
// focus-rect feedback, so those still read correctly despite being
// hand-painted instead of theme-drawn.
void GroupPickerWindow::DrawOwnerButton(const DRAWITEMSTRUCT& item) {
    const bool dark = IsDarkModeEnabled();
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool isDefault = item.hwndItem == createButton_;

    COLORREF fill;
    COLORREF border;
    COLORREF text;
    if (isDefault) {
        // Solid accent fill, matching Windows 11's own filled-accent
        // primary-button style (e.g. Settings' "Save"/"Apply") -- not
        // just an accent-colored border on an otherwise ordinary gray
        // button, as before.
        const COLORREF accent = GetAccentColor();
        fill = pressed ? DarkenColor(accent, 0.85) : accent;
        border = fill;
        text = RGB(0xFF, 0xFF, 0xFF);
    } else {
        fill = dark ? (pressed ? RGB(0x3A, 0x3A, 0x3A) : RGB(0x2B, 0x2B, 0x2B))
                    : (pressed ? RGB(0xD0, 0xD0, 0xD0) : RGB(0xE1, 0xE1, 0xE1));
        border = dark ? RGB(0x50, 0x50, 0x50) : RGB(0xAD, 0xAD, 0xAD);
        text = dark ? RGB(0xE8, 0xE8, 0xE8) : RGB(0x00, 0x00, 0x00);
    }
    if (disabled) {
        text = dark ? RGB(0x70, 0x70, 0x70) : RGB(0x9E, 0x9E, 0x9E);
    }

    const UINT dpi = GetDpiForWindow(item.hwndItem);
    const int cornerRadius = Scale(kButtonCornerRadius, dpi);

    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, fillBrush);
    HGDIOBJ oldPen = SelectObject(item.hDC, borderPen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, cornerRadius,
              cornerRadius);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);

    wchar_t buttonText[128] = L"";
    GetWindowTextW(item.hwndItem, buttonText, static_cast<int>(sizeof(buttonText) / sizeof(buttonText[0])));
    SetTextColor(item.hDC, text);
    SetBkMode(item.hDC, TRANSPARENT);
    RECT textRect = item.rcItem;
    DrawTextW(item.hDC, buttonText, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused) {
        RECT focusRect = item.rcItem;
        InflateRect(&focusRect, -3, -3);
        SetTextColor(item.hDC, text);
        DrawFocusRect(item.hDC, &focusRect);
    }
}

void GroupPickerWindow::LayoutControls() {
    if (nameEdit_ == nullptr) {
        return;
    }
    // window_ isn't assigned yet the first time this runs (it's called
    // from WM_CREATE, which fires before CreateWindowExW returns) --
    // nameEdit_ is already a real child window by that point, so it's a
    // safe DPI-query fallback.
    const UINT dpi = GetDpiForWindow(window_ != nullptr ? window_ : nameEdit_);
    RECT client{};
    GetClientRect(GetParent(nameEdit_), &client);

    const int margin = Scale(kMargin, dpi);
    const int nameRowHeight = Scale(kNameRowHeight, dpi);
    const int nameLabelWidth = Scale(kNameLabelWidth, dpi);
    const int buttonHeight = Scale(kButtonHeight, dpi);
    const int buttonWidth = Scale(kButtonWidth, dpi);

    MoveWindow(nameLabel_, margin, margin, nameLabelWidth, nameRowHeight, TRUE);

    // Shrink the edit box to one line's actual height and center that
    // within the row -- see MeasureLineHeight's comment for why (a
    // single-line EDIT given a taller rect doesn't reliably center its
    // own text).
    const int editHeight =
        std::min(nameRowHeight, MeasureLineHeight(nameEdit_, reinterpret_cast<HFONT>(GetStockObject(
                                                                  DEFAULT_GUI_FONT))) +
                                     Scale(6, dpi));
    const int editY = margin + (nameRowHeight - editHeight) / 2;
    MoveWindow(nameEdit_, margin + nameLabelWidth, editY, client.right - margin - (margin + nameLabelWidth),
               editHeight, TRUE);

    const int buttonsTop = client.bottom - margin - buttonHeight;
    const int listTop = margin + nameRowHeight + margin;
    const int listBottom = buttonsTop - margin;
    MoveWindow(list_.WindowHandle(), margin, listTop, client.right - 2 * margin, listBottom - listTop, TRUE);

    // Standard dialog pairing: Cancel at the far right, the primary
    // action immediately to its left.
    MoveWindow(cancelButton_, client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(createButton_, client.right - 2 * margin - 2 * buttonWidth, buttonsTop, buttonWidth, buttonHeight,
               TRUE);
}

void GroupPickerWindow::PopulateList() {
    std::vector<HWND> candidates;
    EnumWindows(EnumPickerCandidatesProc, reinterpret_cast<LPARAM>(&candidates));

    // Existing group members are always kept checked even if they'd
    // normally be filtered out of `candidates` (e.g. currently
    // minimized) -- editing membership should never silently drop a
    // member just because of a transient state at edit time (see
    // GroupPickerListWindow::SetWindows).
    std::vector<HWND> validInitial;
    for (HWND hwnd : initialSelection_) {
        if (IsWindow(hwnd)) {
            validInitial.push_back(hwnd);
        }
    }
    list_.SetWindows(candidates, validInitial);
}

// Create Group needs at least one checked window; disabled otherwise so
// the button's own state communicates whether clicking it would do
// anything, rather than it being a silent no-op.
void GroupPickerWindow::UpdateButtonStates() {
    if (createButton_ == nullptr) {
        return;
    }
    EnableWindow(createButton_, !list_.CheckedWindows().empty());
}

void GroupPickerWindow::Commit() {
    wchar_t nameBuffer[256] = L"";
    GetWindowTextW(nameEdit_, nameBuffer, static_cast<int>(sizeof(nameBuffer) / sizeof(nameBuffer[0])));
    std::wstring name = nameBuffer;
    if (name.empty()) {
        name = editing_ ? initialName_ : L"New Group";  // never confirm an empty name
    }
    const std::vector<HWND> windows = list_.CheckedWindows();
    LogDebug(std::format(L"[Polish] GroupPicker: confirmed with {} window(s), name=\"{}\"", windows.size(), name));
    result_ = GroupPickerResult{windows, name};
    done_ = true;
}

std::optional<GroupPickerResult> GroupPickerWindow::ShowModal(HWND owner, const std::vector<HWND>& initialSelection,
                                                                const std::wstring& initialName, bool editing) {
    initialSelection_ = initialSelection;
    initialName_ = initialName;
    editing_ = editing;

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

    // Same title whether creating or editing -- both are the same
    // underlying action (choose which windows belong to the group), so
    // one consistent name is used everywhere it's referenced (the
    // taskbar menu, this title, the chrome's own context menu item).
    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName, L"Edit Group Windows",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr, instance_,
                               this);
    if (window_ == nullptr) {
        return std::nullopt;
    }

    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);

    done_ = false;
    result_.reset();
    MSG msg;
    while (!done_) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            (msg.hwnd == window_ || IsChild(window_, msg.hwnd))) {
            result_.reset();
            done_ = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // DestroyWindow recursively destroys every child, including list_'s
    // own window -- GroupPickerListWindow's WM_NCDESTROY handler clears
    // its own window_ back to nullptr as part of that, so list_ is
    // already safely reset by the time this returns (see its own
    // comment); nothing extra needed here for it.
    DestroyWindow(window_);
    window_ = nullptr;
    nameLabel_ = nullptr;
    nameEdit_ = nullptr;
    createButton_ = nullptr;
    cancelButton_ = nullptr;
    if (backgroundBrush_ != nullptr) {
        DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
    if (editBackgroundBrush_ != nullptr) {
        DeleteObject(editBackgroundBrush_);
        editBackgroundBrush_ = nullptr;
    }
    return result_;
}

}  // namespace polish
