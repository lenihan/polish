#include "hook/GroupPickerWindow.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <format>
#include <iterator>

#include "hook/GroupChromeWindow.h"
#include "util/DarkMode.h"
#include "util/DialogKeyboard.h"
#include "util/Logging.h"
#include "util/UiFont.h"
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

// Polls for windows opening/closing while the dialog sits open (see
// RefreshCandidates) -- no push-based hook for this exists anywhere in
// this codebase to reuse instead (confirmed: no EVENT_OBJECT_CREATE/
// SHOW/HIDE hook exists), so this mirrors the interval this codebase's
// other idle polling timers already use (kThumbnailRefreshDelayMs = 1200
// in main.cpp) rather than inventing an unrelated cadence.
constexpr UINT_PTR kCandidateRefreshTimerId = 1;
constexpr UINT kCandidateRefreshIntervalMs = 1000;

// Logical (96 DPI) layout constants -- scaled by the window's actual DPI
// in LayoutControls/before CreateWindowExW. Wide and short, like this
// dialog's original two-list side-by-side layout (see
// GroupPickerWindow.h's own comment on the side-by-side split) -- not
// the narrow/tall shape a single stacked list needed; margin/button
// height still match Windows 11's airier spacing versus this dialog's
// original, denser layout.
constexpr int kWindowWidth = 720;
constexpr int kWindowHeight = 480;
constexpr int kMargin = 20;
constexpr int kColumnGap = 16;
constexpr int kButtonHeight = 32;
constexpr int kButtonWidth = 110;
constexpr int kButtonCornerRadius = 6;

// Name field: a small muted caption above a full-width rounded field
// card, Windows 11 Settings' own "label above the control" convention
// -- not the label-to-the-left-of-a-boxed-field layout this dialog used
// before, which read as an older Win32 dialog convention. The real
// nameEdit_ HWND itself stays small (just its own text height, see
// MeasureLineHeight's comment) and is centered inside the taller,
// hand-drawn kNameFieldHeight card (WM_ERASEBKGND draws the
// rounded-rect card; nameEdit_ has no border/background of its own
// anymore) -- the same "give the control only the height it needs and
// center that" trick as before, just now centered within a nicer-
// looking card instead of a plain bordered rectangle.
constexpr int kNameCaptionHeight = 16;
constexpr int kNameCaptionGap = 6;
constexpr int kNameFieldHeight = 36;
constexpr int kNameFieldPaddingX = 12;

// Available windows/Group column captions -- same small-muted-label
// convention as the Name field's own caption.
constexpr int kSectionCaptionHeight = 16;
constexpr int kSectionCaptionGap = 6;

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
// *that* short control within its layout row -- no subclassing needed.
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
    // Other groups' chrome windows are deliberately *not* excluded here
    // anymore. A chrome is a perfectly normal top-level window
    // everywhere else in this app, and excluding every one of them (as
    // an earlier version did, by class name) left a real hole:
    // a window that's already a member of some group is WS_CHILD and
    // therefore invisible to EnumWindows entirely (see
    // WindowReparenting.h), so with its group's chrome filtered out too,
    // an entire group's worth of windows had *no* representation in this
    // list at all -- confirmed, human-reported, with a grouped Explorer
    // window that simply couldn't be found anywhere. Listing the chrome
    // gives that group a single visible entry standing in for all of it.
    // Only the group currently being edited is excluded, and that
    // happens in PopulateLists/RefreshCandidates (which know which one
    // that is), not here.
    //
    // IsCandidateWindowShape, not IsCandidateWindow -- this dialog has
    // no separate "minimized" section the way Alt+Tab does, so it can't
    // afford to drop minimized windows from its one and only list the
    // way Alt+Tab's main cycle does (confirmed, human-reported: Notepad/
    // Explorer/Outlook windows minimized at the time "Open windows" was
    // opened didn't appear at all). A minimized window is exactly as
    // groupable as a restored one -- GroupManager::EnsureReparented
    // restores it on add so it doesn't join as a blank tile.
    if (IsCandidateWindowShape(hwnd) && !IsElevatedWindow(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

}  // namespace

GroupPickerWindow::GroupPickerWindow(HINSTANCE instance) : instance_(instance), selected_(instance), available_(instance) {
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
            PopulateLists();
            return 0;

        case WM_SIZE:
            LayoutControls();
            return 0;

        case WM_DPICHANGED: {
            // Standard MSDN pattern (same as GroupChromeWindow/
            // AltTabListWindow's own WM_DPICHANGED handlers), plus
            // rebuilding dialogFont_ -- unlike those two, this dialog's
            // text doesn't scale on its own via a per-paint DPI query,
            // so a plain resize alone would leave stale-size text in a
            // now-correctly-sized window.
            const auto* suggestedRect = reinterpret_cast<const RECT*>(lParam);
            ApplyDialogFont(HIWORD(wParam));
            SetWindowPos(hwnd, nullptr, suggestedRect->left, suggestedRect->top,
                         suggestedRect->right - suggestedRect->left, suggestedRect->bottom - suggestedRect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);  // -> WM_SIZE -> LayoutControls
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kCandidateRefreshTimerId) {
                RefreshCandidates();
            }
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
            // reports success (see ApplyDarkMode) but no translucent
            // backdrop actually renders on this host, just a flat,
            // undifferentiated light fill behind everything, which reads
            // as a visual bug (a stray white margin around an otherwise
            // fully dark-themed dialog) rather than a subtle backdrop. A
            // correctly opaque, consistent dialog beats a broken-looking
            // one chasing an effect that isn't visibly paying off in
            // practice -- micaEnabled_ (and the DwmSetWindowAttribute
            // calls themselves) are left in place in case a future
            // Windows/driver combination renders it correctly, but
            // nothing here currently depends on that.
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(hdc, &client,
                      backgroundBrush_ != nullptr ? backgroundBrush_
                                                   : reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));

            // The Name field's rounded-rect card -- drawn here (not by
            // nameEdit_ itself, which has no border/background style of
            // its own) so it can extend past the small real EDIT control
            // into a comfortably tall, Windows-11-looking field surface;
            // WM_CTLCOLOREDIT fills nameEdit_'s own rect with the exact
            // same color so the two read as one seamless surface.
            if (editBackgroundBrush_ != nullptr && nameFieldRect_.right > nameFieldRect_.left) {
                const UINT dpi = GetDpiForWindow(hwnd);
                const int cornerRadius = Scale(kButtonCornerRadius, dpi);
                const bool dark = IsDarkModeEnabled();
                HPEN borderPen = CreatePen(PS_SOLID, 1, dark ? RGB(0x50, 0x50, 0x50) : RGB(0xAD, 0xAD, 0xAD));
                HGDIOBJ oldBrush = SelectObject(hdc, editBackgroundBrush_);
                HGDIOBJ oldPen = SelectObject(hdc, borderPen);
                RoundRect(hdc, nameFieldRect_.left, nameFieldRect_.top, nameFieldRect_.right, nameFieldRect_.bottom,
                          cornerRadius, cornerRadius);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(borderPen);
            }
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
            // Muted/secondary text color for every caption (Name,
            // Selected, Available windows) -- a field label, not body
            // text, the same visual hierarchy Windows 11 Settings uses
            // between a section's label and its value/content.
            // Background matches the dialog's own (captions sit
            // directly on it, not on the rounded field card below the
            // Name one).
            HDC hdcStatic = reinterpret_cast<HDC>(wParam);
            const bool dark = IsDarkModeEnabled();
            SetTextColor(hdcStatic, dark ? RGB(0xA0, 0xA0, 0xA0) : RGB(0x60, 0x60, 0x60));
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
    // A small muted caption above the field, not a colon-suffixed
    // prompt beside it -- see kNameCaptionHeight's own comment on why.
    nameLabel_ =
        CreateWindowExW(0, L"STATIC", L"Name", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, nullptr, instance_,
                         nullptr);
    // No border/background style of its own -- WM_ERASEBKGND draws a
    // rounded-rect card behind it (see kNameCaptionHeight's own
    // comment), and WM_CTLCOLOREDIT fills this control's own small rect
    // with the exact same color so the two blend seamlessly into one
    // surface with no visible seam between "card" and "control."
    nameEdit_ = CreateWindowExW(0, L"EDIT", initialName_.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                 hwnd, nullptr, instance_, nullptr);

    selectedLabel_ = CreateWindowExW(0, L"STATIC", L"Group", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                                      nullptr, instance_, nullptr);
    selected_.Create(hwnd);
    // The single source of truth is selectedOrder_ (see class comment);
    // every callback just mutates it and re-pushes it down to both
    // lists via RefreshLists.
    selected_.SetOnReordered([this](std::vector<HWND> newOrder) {
        selectedOrder_ = std::move(newOrder);
        RefreshLists();
    });
    selected_.SetOnRemoveRequested([this](HWND hwnd2) {
        const auto it = std::find(selectedOrder_.begin(), selectedOrder_.end(), hwnd2);
        // Index within "Group" -- the list this window is *leaving* --
        // captured before the erase so the row that slides into its old
        // slot can be worked out below.
        const size_t leavingIndex =
            it != selectedOrder_.end() ? static_cast<size_t>(it - selectedOrder_.begin()) : 0;
        if (it != selectedOrder_.end()) {
            selectedOrder_.erase(it);
        }
        // Selection stays in "Group" so a run of removes is one keypress
        // (or one click) each, rather than bouncing focus over to
        // "Available windows" after every single one -- an earlier
        // version followed the moved window into its new list instead,
        // reported as needing a Tab/click back to "Group" before the
        // next removal could happen.
        if (!selectedOrder_.empty()) {
            selectedWindow_ = selectedOrder_[std::min(leavingIndex, selectedOrder_.size() - 1)];
            RefreshLists();
            SetFocus(selected_.WindowHandle());
        } else {
            // Nothing left in "Group" to select -- fall back to the
            // window that just left, now sitting in "Available windows".
            selectedWindow_ = hwnd2;
            RefreshLists();
            SetFocus(available_.WindowHandle());
        }
    });
    // A plain row-body click (not Remove, not the grip) just selects
    // that row -- selectedWindow_ is the single source of truth for
    // selection across both lists (see its own comment), so both
    // callbacks below are identical regardless of which list reported
    // the click.
    auto onRowSelected = [this](HWND hwnd2) {
        selectedWindow_ = hwnd2;
        available_.SetSelectedHwnd(selectedWindow_);
        selected_.SetSelectedHwnd(selectedWindow_);
    };
    selected_.SetOnRowSelected(onRowSelected);

    // "Open windows," not "Available windows" -- matches this dialog's
    // original (pre-Windows-11-redesign) wording; "available" and
    // "open" mean the same thing here, but "open" is the word the
    // dialog used before and nothing about this rework changes that.
    availableLabel_ = CreateWindowExW(0, L"STATIC", L"Open windows", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                       hwnd, nullptr, instance_, nullptr);
    available_.Create(hwnd);
    available_.SetOnAddRequested([this](HWND hwnd2) {
        // Index within "Available windows" -- the list this window is
        // *leaving* -- captured before the mutation, same reasoning as
        // the symmetric block in selected_'s own SetOnRemoveRequested.
        const std::vector<HWND> leavingList = AvailableWindows();
        const auto leavingIt = std::find(leavingList.begin(), leavingList.end(), hwnd2);
        const size_t leavingIndex =
            leavingIt != leavingList.end() ? static_cast<size_t>(leavingIt - leavingList.begin()) : 0;
        if (std::find(selectedOrder_.begin(), selectedOrder_.end(), hwnd2) == selectedOrder_.end()) {
            selectedOrder_.push_back(hwnd2);
        }
        // Selection stays in "Available windows" so a run of adds is one
        // keypress (or one click) each -- see the symmetric comment on
        // selected_'s own SetOnRemoveRequested for why this replaced the
        // earlier "follow the window into its new list" behavior.
        const std::vector<HWND> remaining = AvailableWindows();
        if (!remaining.empty()) {
            selectedWindow_ = remaining[std::min(leavingIndex, remaining.size() - 1)];
            RefreshLists();
            SetFocus(available_.WindowHandle());
        } else {
            // Nothing left in "Available windows" -- fall back to the
            // window that just left, now sitting in "Group".
            selectedWindow_ = hwnd2;
            RefreshLists();
            SetFocus(selected_.WindowHandle());
        }
    });
    available_.SetOnRowSelected(onRowSelected);

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

    // dialogFont_ must exist before LayoutControls runs -- it measures
    // nameEdit_'s line height against it (see MeasureLineHeight's own
    // comment). Real sizing (LayoutControls) still runs before
    // WM_SETFONT, not after -- a single-line EDIT control's internal
    // vertical-text-centering offset is computed relative to its size at
    // the time it receives WM_SETFONT, not recomputed on a later resize.
    // Every control here is created at a placeholder 0x0 size, so
    // sending WM_SETFONT before the real MoveWindow left the name
    // field's text pinned to the top instead of centered.
    dialogFont_ = MakeUiFont(GetDpiForWindow(hwnd));
    LayoutControls();

    for (HWND control : {nameLabel_, nameEdit_, selectedLabel_, availableLabel_, createButton_, cancelButton_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont_), TRUE);
    }

    ApplyDarkMode();
}

// Rebuilds dialogFont_ at dpi and re-pushes WM_SETFONT to every control
// that uses it -- called once from CreateControls (dpi already current)
// and again on every WM_DPICHANGED (moving to a different-DPI monitor).
// The new font is created and pushed before the old one is freed: for
// the moment in between, every control must keep holding a valid HFONT,
// never a deleted one.
void GroupPickerWindow::ApplyDialogFont(UINT dpi) {
    HFONT newFont = MakeUiFont(dpi);
    for (HWND control : {nameLabel_, nameEdit_, selectedLabel_, availableLabel_, createButton_, cancelButton_}) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
        }
    }
    if (dialogFont_ != nullptr) {
        DeleteObject(dialogFont_);
    }
    dialogFont_ = newFont;
}

void GroupPickerWindow::CycleFocus(bool backward) {
    // Order is the dialog's own reading order: the name field, then the
    // two panels left-to-right, then the two buttons. The ring mechanics
    // (skipping disabled stops -- Create Group is disabled whenever the
    // group is empty, see UpdateButtonStates -- and wrapping) live in
    // util/DialogKeyboard, shared with GroupHotkeyDialog.
    const HWND stops[] = {nameEdit_, available_.WindowHandle(), selected_.WindowHandle(), createButton_,
                          cancelButton_};
    polish::CycleFocus(stops, std::size(stops), backward);
}

// Applies (or re-applies, on a live WM_SETTINGCHANGE) the current OS
// theme to the whole dialog: the native title bar, cached background
// brushes backing WM_ERASEBKGND/WM_CTLCOLORSTATIC/WM_CTLCOLOREDIT, and
// every themeable child control. Both list panels read
// IsDarkModeEnabled() fresh at their own paint time, so they just need
// a repaint here, not separate state pushed down to them.
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
    // SetWindowTheme -- see their creation comment for why. The edit
    // field needs it for its scrollbar/selection colors (not its
    // border, which is hand-drawn now -- see its creation comment);
    // both list panels need it for the same reason -- their WS_VSCROLL
    // scrollbars are native controls that don't follow dark mode on
    // their own (their rows are hand-painted and already do, via
    // IsDarkModeEnabled() read fresh in their own Paint -- only the
    // OS-drawn scrollbar track/thumb needed this).
    if (nameEdit_ != nullptr) {
        SetWindowTheme(nameEdit_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    if (selected_.WindowHandle() != nullptr) {
        SetWindowTheme(selected_.WindowHandle(), dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    if (available_.WindowHandle() != nullptr) {
        SetWindowTheme(available_.WindowHandle(), dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }

    // RDW_ALLCHILDREN, not a plain InvalidateRect -- invalidating just
    // the dialog itself doesn't propagate to child windows (each has
    // its own update region), which would leave the owner-drawn
    // buttons/panels stuck showing the old theme's colors after a live
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
        if (disabled) {
            // Plain gray text (the non-default disabled treatment
            // below) on a full-brightness accent fill was illegible --
            // confirmed, human-reported. Dim the fill itself instead
            // (the same DarkenColor helper already used for the
            // pressed state), and keep the text light rather than
            // switching it to gray, so it still reads clearly against
            // the now-muted fill.
            fill = DarkenColor(accent, 0.4);
            text = RGB(0xC8, 0xC8, 0xC8);
        } else {
            fill = pressed ? DarkenColor(accent, 0.85) : accent;
            text = RGB(0xFF, 0xFF, 0xFF);
        }
        border = fill;
    } else {
        fill = dark ? (pressed ? RGB(0x3A, 0x3A, 0x3A) : RGB(0x2B, 0x2B, 0x2B))
                    : (pressed ? RGB(0xD0, 0xD0, 0xD0) : RGB(0xE1, 0xE1, 0xE1));
        border = dark ? RGB(0x50, 0x50, 0x50) : RGB(0xAD, 0xAD, 0xAD);
        text = dark ? RGB(0xE8, 0xE8, 0xE8) : RGB(0x00, 0x00, 0x00);
        if (disabled) {
            text = dark ? RGB(0x70, 0x70, 0x70) : RGB(0x9E, 0x9E, 0x9E);
        }
    }

    const UINT dpi = GetDpiForWindow(item.hwndItem);
    const int cornerRadius = Scale(kButtonCornerRadius, dpi);

    // BS_OWNERDRAW buttons still get a default WM_ERASEBKGND from
    // DefWindowProc before WM_DRAWITEM ever runs, using the stock
    // "Button" class background (COLOR_BTNFACE -- a light, undarkened
    // gray) -- confirmed via pixel sampling a live screenshot: a ~1-2px
    // sliver of exactly that color showed at each corner, precisely
    // where RoundRect's own arc leaves the bounding box's four corner
    // triangles untouched by the fill below. Painting the dialog's own
    // background color across the *entire* rect first, before the
    // rounded fill on top, guarantees those triangles read as
    // background instead of stray light gray, matching the same
    // full-bleed-then-rounded-fill-on-top order the row buttons in
    // GroupPickerListWindow/GroupPickerSelectedListWindow already use.
    HBRUSH surroundingBrush = CreateSolidBrush(dark ? RGB(0x20, 0x20, 0x20) : GetSysColor(COLOR_3DFACE));
    FillRect(item.hDC, &item.rcItem, surroundingBrush);
    DeleteObject(surroundingBrush);

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
    // The system generally pre-selects the WM_SETFONT font into an
    // owner-drawn button's DC, but that isn't a documented guarantee,
    // and button text is the most visible half of the DPI-font bug this
    // was written to fix -- select dialogFont_ explicitly rather than
    // rely on it.
    HGDIOBJ oldButtonFont = SelectObject(item.hDC, dialogFont_);
    RECT textRect = item.rcItem;
    DrawTextW(item.hDC, buttonText, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, oldButtonFont);

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
    const int captionHeight = Scale(kNameCaptionHeight, dpi);
    const int captionGap = Scale(kNameCaptionGap, dpi);
    const int fieldHeight = Scale(kNameFieldHeight, dpi);
    const int fieldPaddingX = Scale(kNameFieldPaddingX, dpi);
    const int buttonHeight = Scale(kButtonHeight, dpi);
    const int buttonWidth = Scale(kButtonWidth, dpi);
    const int sectionCaptionHeight = Scale(kSectionCaptionHeight, dpi);
    const int sectionCaptionGap = Scale(kSectionCaptionGap, dpi);
    const int columnGap = Scale(kColumnGap, dpi);

    MoveWindow(nameLabel_, margin, margin, client.right - 2 * margin, captionHeight, TRUE);

    const int fieldTop = margin + captionHeight + captionGap;
    nameFieldRect_ = RECT{margin, fieldTop, client.right - margin, fieldTop + fieldHeight};

    // Shrink the edit box to one line's actual height and center that
    // within the (taller, decorative) field card -- see
    // MeasureLineHeight's comment for why (a single-line EDIT given a
    // taller rect doesn't reliably center its own text). No extra
    // padding added to the measured line height (an earlier version
    // added +6px, tuned for the old, shorter kNameRowHeight layout --
    // confirmed, human-reported as no longer centered once this field
    // became a taller standalone card): the raw text metrics already
    // give the control the height it needs.
    const int editHeight = std::min(fieldHeight, MeasureLineHeight(nameEdit_, dialogFont_));
    const int editY = nameFieldRect_.top + (fieldHeight - editHeight) / 2;
    MoveWindow(nameEdit_, nameFieldRect_.left + fieldPaddingX, editY,
               (nameFieldRect_.right - nameFieldRect_.left) - 2 * fieldPaddingX, editHeight, TRUE);

    // Two side-by-side columns, equal width -- "Open windows" (left,
    // available_) and "Group" (right, selected_) -- both the same
    // height, filling whatever's left down to the button row. Neither
    // column's size depends on how many rows it currently holds (unlike
    // an earlier stacked version, where the Selected panel visibly
    // resizing the whole dialog on every selection change read as
    // jank); each just gets its own scrollbar if its content overflows
    // this fixed height.
    const int columnWidth = (client.right - 2 * margin - columnGap) / 2;
    const int leftColumnLeft = margin;
    const int rightColumnLeft = leftColumnLeft + columnWidth + columnGap;

    const int columnCaptionTop = nameFieldRect_.bottom + margin;
    MoveWindow(availableLabel_, leftColumnLeft, columnCaptionTop, columnWidth, sectionCaptionHeight, TRUE);
    MoveWindow(selectedLabel_, rightColumnLeft, columnCaptionTop, columnWidth, sectionCaptionHeight, TRUE);

    const int columnsTop = columnCaptionTop + sectionCaptionHeight + sectionCaptionGap;
    const int buttonsTop = client.bottom - margin - buttonHeight;
    const int columnsBottom = buttonsTop - margin;
    MoveWindow(available_.WindowHandle(), leftColumnLeft, columnsTop, columnWidth, columnsBottom - columnsTop, TRUE);
    MoveWindow(selected_.WindowHandle(), rightColumnLeft, columnsTop, columnWidth, columnsBottom - columnsTop, TRUE);

    // Standard dialog pairing: Cancel at the far right, the primary
    // action immediately to its left.
    MoveWindow(cancelButton_, client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(createButton_, client.right - 2 * margin - 2 * buttonWidth, buttonsTop, buttonWidth, buttonHeight,
               TRUE);
}

void GroupPickerWindow::PopulateLists() {
    allCandidates_.clear();
    EnumWindows(EnumPickerCandidatesProc, reinterpret_cast<LPARAM>(&allCandidates_));
    // Not visible yet at this point (WM_CREATE fires before ShowWindow),
    // so EnumPickerCandidatesProc's own IsCandidateWindowShape check
    // already excludes window_ here in practice -- this erase is just
    // defensive symmetry with RefreshCandidates' own identical line,
    // where it's load-bearing (see that method's comment).
    std::erase(allCandidates_, window_);
    std::erase(allCandidates_, editedGroupChrome_);
    LogCandidates(L"populate");

    // Existing group members are always kept selected even if they'd
    // normally be filtered out of allCandidates_ (e.g. currently
    // minimized) -- editing membership should never silently drop a
    // member just because of a transient state at edit time.
    selectedOrder_.clear();
    for (HWND hwnd : initialSelection_) {
        if (IsWindow(hwnd)) {
            selectedOrder_.push_back(hwnd);
        }
    }
    // Reset explicitly rather than relying on RefreshLists' own fallback
    // to overwrite a stale value -- this object is reused across
    // multiple ShowModal calls, and a leftover selection from a
    // previous dialog session could otherwise coincidentally still
    // refer to a window that's (still) open now, silently carrying a
    // stale selection into what should be a fresh dialog.
    selectedWindow_.reset();
    RefreshLists();
}

void GroupPickerWindow::LogCandidates(const wchar_t* reason) const {
    std::wstring dump;
    for (HWND hwnd : allCandidates_) {
        wchar_t title[128] = L"";
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        wchar_t className[128] = L"";
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (!dump.empty()) {
            dump += L" | ";
        }
        // Class name included for the same reason main.cpp's Alt+Tab
        // dump includes it: it's what distinguishes a real app window
        // from a UWP host frame (ApplicationFrameWindow,
        // Windows.UI.Core.CoreWindow) and from another group's own
        // chrome (PolishGroupChromeWindow), which is now a legitimate
        // candidate rather than a filtered-out one.
        dump += std::format(L"{}:\"{}\"[{}]", reinterpret_cast<void*>(hwnd), title, className);
    }
    LogDebug(std::format(L"[Polish] Picker: {} -- {} candidate(s): {}", reason, allCandidates_.size(), dump));
}

std::vector<HWND> GroupPickerWindow::AvailableWindows() const {
    // Mutually exclusive: a window shows in exactly one of the two
    // panels (see class comment) -- available_ gets allCandidates_
    // minus whatever's currently in selectedOrder_, in allCandidates_'s
    // own stable order.
    std::vector<HWND> availableWindows;
    availableWindows.reserve(allCandidates_.size());
    for (HWND hwnd : allCandidates_) {
        if (std::find(selectedOrder_.begin(), selectedOrder_.end(), hwnd) == selectedOrder_.end()) {
            availableWindows.push_back(hwnd);
        }
    }
    return availableWindows;
}

void GroupPickerWindow::RefreshLists() {
    const std::vector<HWND> availableWindows = AvailableWindows();
    available_.SetWindows(availableWindows);
    selected_.SetSelected(selectedOrder_);
    UpdateButtonStates();

    // Fall back to a sensible default whenever selectedWindow_ is unset
    // or no longer refers to a window in either list (closed, or dropped
    // by a live-refresh reconciliation -- see RefreshCandidates) -- the
    // same "always show a selected row so its action button stays
    // discoverable" reasoning an earlier version's row-0 default served,
    // just computed here (once, for both lists) instead of inside a
    // child list guessing its own default independently.
    const bool stillValid =
        selectedWindow_.has_value() &&
        (std::find(availableWindows.begin(), availableWindows.end(), *selectedWindow_) != availableWindows.end() ||
         std::find(selectedOrder_.begin(), selectedOrder_.end(), *selectedWindow_) != selectedOrder_.end());
    if (!stillValid) {
        if (!availableWindows.empty()) {
            selectedWindow_ = availableWindows.front();
        } else if (!selectedOrder_.empty()) {
            selectedWindow_ = selectedOrder_.front();
        } else {
            selectedWindow_.reset();
        }
    }
    available_.SetSelectedHwnd(selectedWindow_);
    selected_.SetSelectedHwnd(selectedWindow_);
}

void GroupPickerWindow::RefreshCandidates() {
    if (selected_.IsDragging()) {
        return;  // don't rebuild rows_ out from under an in-progress drag
    }

    std::vector<HWND> freshCandidates;
    EnumWindows(EnumPickerCandidatesProc, reinterpret_cast<LPARAM>(&freshCandidates));
    // Unlike PopulateLists' own initial enumeration (which runs from
    // WM_CREATE, before ShowWindow makes window_ visible), this timer-
    // driven re-enumeration runs while the dialog is already shown --
    // window_ itself is by then a real, visible, WS_CAPTION-having,
    // ownerless top-level window with non-empty text ("Edit Group
    // Windows"), satisfying every check IsCandidateWindowShape makes.
    // Confirmed, human-reported: without this, the dialog starts
    // listing itself as a candidate in its own "Open windows" a tick or
    // so after opening.
    std::erase(freshCandidates, window_);
    std::erase(freshCandidates, editedGroupChrome_);

    // Order-preserving merge, the same way src/main.cpp's
    // UpdateAltTabCandidatesPreservingOrder reconciles Alt+Tab's own
    // candidate list mid-session: survivors keep their existing
    // position (comparing membership, not the fresh EnumWindows Z-order,
    // avoids reshuffling Available's row order just because focus
    // changed elsewhere on the desktop), closed ones drop out, brand-new
    // ones append at the end.
    std::vector<HWND> updated;
    for (HWND hwnd : allCandidates_) {
        if (std::find(freshCandidates.begin(), freshCandidates.end(), hwnd) != freshCandidates.end()) {
            updated.push_back(hwnd);
        }
    }
    for (HWND hwnd : freshCandidates) {
        if (std::find(updated.begin(), updated.end(), hwnd) == updated.end()) {
            updated.push_back(hwnd);
        }
    }

    const size_t priorSelectedCount = selectedOrder_.size();
    std::erase_if(selectedOrder_, [](HWND h) { return !IsWindow(h); });

    // A no-op skips RefreshLists entirely, not just as an optimization:
    // SetWindows/SetSelected unconditionally reset scrollOffset_/
    // hoveredIndex_ and re-fetch every row's icon (GetWindowIconHandle's
    // packaged-app fallback intentionally *leaks* the icon it allocates
    // -- an accepted tradeoff for a handful of dialog-lifetime icons,
    // not for one leaked every second this timer ticks with nothing
    // actually changed).
    if (updated == allCandidates_ && selectedOrder_.size() == priorSelectedCount) {
        return;
    }
    allCandidates_ = std::move(updated);
    RefreshLists();
}

// Create Group needs at least one selected window; disabled otherwise
// so the button's own state communicates whether clicking it would do
// anything, rather than it being a silent no-op.
void GroupPickerWindow::UpdateButtonStates() {
    if (createButton_ == nullptr) {
        return;
    }
    EnableWindow(createButton_, !selectedOrder_.empty());
}

void GroupPickerWindow::Commit() {
    wchar_t nameBuffer[256] = L"";
    GetWindowTextW(nameEdit_, nameBuffer, static_cast<int>(sizeof(nameBuffer) / sizeof(nameBuffer[0])));
    std::wstring name = nameBuffer;
    if (name.empty()) {
        name = editing_ ? initialName_ : L"New Group";  // never confirm an empty name
    }
    LogDebug(std::format(L"[Polish] GroupPicker: confirmed with {} window(s), name=\"{}\"", selectedOrder_.size(),
                          name));
    result_ = GroupPickerResult{selectedOrder_, name};
    done_ = true;
}

std::optional<GroupPickerResult> GroupPickerWindow::ShowModal(HWND owner, const std::vector<HWND>& initialSelection,
                                                                const std::wstring& initialName, bool editing) {
    initialSelection_ = initialSelection;
    initialName_ = initialName;
    editing_ = editing;
    // When editing, `owner` *is* the edited group's own chrome (main.cpp
    // passes it); when creating a new group it's nullptr and nothing
    // gets excluded. See editedGroupChrome_'s own comment.
    editedGroupChrome_ = owner;

    HMONITOR monitor = (owner != nullptr && IsWindow(owner)) ? MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY)
                                                               : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    // GetDpiForMonitor(monitor), not GetDpiForSystem() -- this dialog is
    // about to appear on monitor, which may not be the one system DPI
    // describes (e.g. owner sits on a different-DPI secondary display).
    // Same reasoning AltTabListWindow::Show already applies for its own
    // per-monitor Reposition call, and the same create-time bug
    // GroupChromeWindow's own constructor already found and fixed for
    // itself.
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const int width = Scale(kWindowWidth, dpiX);
    const int height = Scale(kWindowHeight, dpiX);

    RECT monitorRect{};
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
    // Start on the name field -- the first of CycleFocus's five tab stops,
    // so the very first Tab press moves to "Open windows" rather than
    // landing somewhere arbitrary, and typing a group name works without
    // having to click the field first.
    SetFocus(nameEdit_);

    // Live-refreshes allCandidates_/selectedOrder_ against currently-open
    // windows while the dialog sits idle (see RefreshCandidates) -- no
    // push-based hook for window-open/close exists to reuse instead.
    SetTimer(window_, kCandidateRefreshTimerId, kCandidateRefreshIntervalMs, nullptr);

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
        // Enter commits, the counterpart to Escape above. Needed by hand
        // for the same reason Tab is: the Create button is BS_OWNERDRAW
        // with no dialog manager behind it, so BS_DEFPUSHBUTTON would do
        // nothing and Enter was simply dead everywhere in the dialog --
        // including in the name field, where it's the most natural way
        // to finish. Ignored while Create is disabled (an empty group),
        // matching what clicking the button would do.
        if (IsDialogKeyDown(msg, window_, VK_RETURN)) {
            if (createButton_ != nullptr && IsWindowEnabled(createButton_)) {
                Commit();
            }
            continue;
        }
        // Swallowed here, before TranslateMessage/DispatchMessageW, so the
        // name field never receives it as a literal tab character -- the
        // same interception point and shape as the Escape case above. This
        // window is deliberately not a real dialog (see the class
        // comment), so Windows' own Tab handling never runs; CycleFocus
        // does it by hand.
        if (IsDialogKeyDown(msg, window_, VK_TAB)) {
            CycleFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(window_, kCandidateRefreshTimerId);

    // DestroyWindow recursively destroys every child, including both
    // list panels' own windows -- their WM_NCDESTROY handlers clear
    // their own window_ back to nullptr as part of that, so they're
    // already safely reset by the time this returns; nothing extra
    // needed here for them.
    DestroyWindow(window_);
    window_ = nullptr;
    nameLabel_ = nullptr;
    nameEdit_ = nullptr;
    selectedLabel_ = nullptr;
    availableLabel_ = nullptr;
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
    if (dialogFont_ != nullptr) {
        DeleteObject(dialogFont_);
        dialogFont_ = nullptr;
    }
    return result_;
}

}  // namespace polish
