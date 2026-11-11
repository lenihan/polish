#include "hook/RestoreAppsDialog.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <filesystem>
#include <iterator>

#include "util/DarkMode.h"
#include "util/DialogKeyboard.h"
#include "util/UiFont.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishRestoreAppsDialog";
constexpr int kListId = 3001;
constexpr int kOkId = 3002;
constexpr int kCancelId = 3003;
constexpr int kSelectAllId = 3004;
constexpr int kSelectNoneId = 3005;
constexpr UINT_PTR kRefreshTimerId = 1;
constexpr UINT kRefreshMs = 1000;

// Logical (96 DPI) layout constants.
constexpr int kWindowWidth = 580;
constexpr int kWindowHeight = 540;
constexpr int kMargin = 20;
constexpr int kHeadingHeight = 28;
constexpr int kHintHeight = 22;
constexpr int kButtonHeight = 32;
constexpr int kButtonWidth = 100;
constexpr int kSmallButtonWidth = 104;
constexpr int kGap = 8;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

std::wstring AppLabel(const SavedApp& app) {
    if (!app.aumid.empty()) {
        return L"Microsoft Store app";
    }
    return std::filesystem::path(app.exe).stem().wstring();
}

}  // namespace

RestoreAppsDialog::RestoreAppsDialog(HINSTANCE instance) : instance_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
}

RestoreAppsDialog::~RestoreAppsDialog() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK RestoreAppsDialog::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    RestoreAppsDialog* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<RestoreAppsDialog*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RestoreAppsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT RestoreAppsDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT themed = 0;
    if (theme_.Handle(hwnd, message, wParam, &themed)) {
        return themed;
    }
    switch (message) {
        case WM_CREATE:
            window_ = hwnd;
            CreateControls(hwnd);
            return 0;

        case WM_SIZE:
            LayoutControls();
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case kOkId:
                        Commit();
                        break;
                    case kCancelId:
                        result_.reset();
                        done_ = true;
                        break;
                    case kSelectAllId:
                        SetCheckedAll(true);
                        break;
                    case kSelectNoneId:
                        SetCheckedAll(false);
                        break;
                }
            }
            return 0;

        case WM_TIMER:
            if (wParam == kRefreshTimerId) {
                DropOpenedApps();
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_CLOSE:
            result_.reset();
            done_ = true;
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void RestoreAppsDialog::CreateControls(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    font_ = MakeUiFont(dpi);
    headingFont_ = MakeUiFontWithWeight(dpi, FW_SEMIBOLD);

    auto child = [&](const wchar_t* className, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
        return CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };

    heading_ = child(L"STATIC", L"Reopen your apps?", SS_LEFT, 0);
    hint_ = child(L"STATIC", L"These windows were open last time and are not open now. Untick any you don't want.",
                  SS_LEFT | SS_ENDELLIPSIS, 0);
    list_ = child(WC_LISTVIEWW, L"",
                  LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP,
                  kListId, WS_EX_CLIENTEDGE);
    // The Explorer style is what gives a list the Windows 11 look: the
    // rounded, translucent selection and the modern checkbox. Dark mode swaps
    // it for its dark counterpart in WindowTheme::Apply below.
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                 LVS_EX_LABELTIP);
    selectAll_ = child(L"BUTTON", L"Select all", BS_PUSHBUTTON | WS_TABSTOP, kSelectAllId);
    selectNone_ = child(L"BUTTON", L"Select none", BS_PUSHBUTTON | WS_TABSTOP, kSelectNoneId);
    okButton_ = child(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, kOkId);
    cancelButton_ = child(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, kCancelId);

    for (HWND control : {hint_, list_, selectAll_, selectNone_, okButton_, cancelButton_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);

    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = 100;
    ListView_InsertColumn(list_, 0, &column);
    ListView_InsertColumn(list_, 1, &column);

    FillList();
    LayoutControls();
}

void RestoreAppsDialog::FillList() {
    SHFILEINFOW info{};
    HIMAGELIST systemList = nullptr;
    for (size_t i = 0; i < apps_.size(); ++i) {
        const SavedApp& app = apps_[i];
        int image = -1;
        if (!app.exe.empty()) {
            const auto list = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
                app.exe.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
            if (list != nullptr) {
                systemList = list;
                image = info.iIcon;
            }
        }
        if (systemList != nullptr && i == 0) {
            ListView_SetImageList(list_, systemList, LVSIL_SMALL);
        }
        std::wstring title = app.title.empty() ? AppLabel(app) : app.title;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = static_cast<int>(i);
        item.pszText = title.data();
        item.iImage = image;
        ListView_InsertItem(list_, &item);
        std::wstring label = AppLabel(app);
        ListView_SetItemText(list_, static_cast<int>(i), 1, label.data());
    }
    SetCheckedAll(true);
    if (!apps_.empty()) {
        ListView_SetItemState(list_, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    }
}

void RestoreAppsDialog::SetCheckedAll(bool checked) {
    for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
        ListView_SetCheckState(list_, i, checked);
    }
}

// Takes off the list the apps that have been opened since it was filled: asking
// to reopen something already open would just start a second copy. What the
// user has ticked or unticked on the rest is left alone.
void RestoreAppsDialog::DropOpenedApps() {
    if (!stillMissing_ || apps_.empty()) {
        return;
    }
    const std::vector<bool> missing = stillMissing_(apps_);
    if (missing.size() != apps_.size()) {
        return;
    }
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    for (size_t i = apps_.size(); i-- > 0;) {
        if (!missing[i]) {
            ListView_DeleteItem(list_, static_cast<int>(i));
            apps_.erase(apps_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    if (apps_.empty()) {
        result_.reset();  // nothing left to open
        done_ = true;
    }
}

void RestoreAppsDialog::LayoutControls() {
    if (list_ == nullptr || window_ == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    RECT client{};
    GetClientRect(window_, &client);
    const int margin = Scale(kMargin, dpi);
    const int gap = Scale(kGap, dpi);
    const int buttonHeight = Scale(kButtonHeight, dpi);
    const int buttonWidth = Scale(kButtonWidth, dpi);
    const int smallWidth = Scale(kSmallButtonWidth, dpi);
    const int width = client.right - 2 * margin;

    int y = margin;
    MoveWindow(heading_, margin, y, width, Scale(kHeadingHeight, dpi), TRUE);
    y += Scale(kHeadingHeight, dpi);
    MoveWindow(hint_, margin, y, width, Scale(kHintHeight, dpi), TRUE);
    y += Scale(kHintHeight, dpi) + gap;

    const int buttonsTop = client.bottom - margin - buttonHeight;
    MoveWindow(list_, margin, y, width, buttonsTop - gap - y, TRUE);
    MoveWindow(selectAll_, margin, buttonsTop, smallWidth, buttonHeight, TRUE);
    MoveWindow(selectNone_, margin + smallWidth + gap, buttonsTop, smallWidth, buttonHeight, TRUE);
    MoveWindow(cancelButton_, client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(okButton_, client.right - margin - 2 * buttonWidth - gap, buttonsTop, buttonWidth, buttonHeight, TRUE);

    RECT listClient{};
    GetClientRect(list_, &listClient);
    const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    const int total = listClient.right - scrollbar;
    ListView_SetColumnWidth(list_, 0, total * 62 / 100);
    ListView_SetColumnWidth(list_, 1, total - total * 62 / 100);
}

void RestoreAppsDialog::Commit() {
    std::vector<SavedApp> chosen;
    for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
        if (ListView_GetCheckState(list_, i) != 0) {
            chosen.push_back(apps_[static_cast<size_t>(i)]);
        }
    }
    result_ = std::move(chosen);
    done_ = true;
}

std::optional<std::vector<SavedApp>> RestoreAppsDialog::ShowModal(const std::vector<SavedApp>& apps,
                                                                       MissingCheck stillMissing) {
    apps_ = apps;
    stillMissing_ = std::move(stillMissing);

    const UINT dpi = GetDpiForSystem();
    const int width = Scale(kWindowWidth, dpi);
    const int height = Scale(kWindowHeight, dpi);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitorInfo)) {
        work = monitorInfo.rcWork;
    }
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;

    // A normal top-level window with a caption and a close button, so Windows
    // 11 gives it the standard rounded frame and shadow.
    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kWindowClassName, L"Polish — reopen apps",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, nullptr, nullptr, instance_,
                              this);
    if (window_ == nullptr) {
        return std::nullopt;
    }
    theme_.Apply(window_);
    // Apply resets a light-mode child to the classic look; the list wants the
    // Explorer style either way.
    SetWindowTheme(list_, theme_.dark() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (theme_.dark()) {
        // Dark mode: the list's own colours (the Explorer dark style does not
        // recolour the client area).
        ListView_SetBkColor(list_, RGB(32, 32, 32));
        ListView_SetTextBkColor(list_, RGB(32, 32, 32));
        ListView_SetTextColor(list_, RGB(240, 240, 240));
    }
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    SetFocus(list_);

    done_ = false;
    result_.reset();
    SetTimer(window_, kRefreshTimerId, kRefreshMs, nullptr);
    const HWND stops[] = {list_, selectAll_, selectNone_, okButton_, cancelButton_};
    MSG msg;
    while (!done_) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            break;
        }
        if (IsDialogKeyDown(msg, window_, VK_ESCAPE)) {
            result_.reset();
            done_ = true;
            break;
        }
        if (IsDialogKeyDown(msg, window_, VK_RETURN)) {
            const HWND focus = GetFocus();
            if (focus == cancelButton_ || focus == selectAll_ || focus == selectNone_) {
                SendMessageW(focus, BM_CLICK, 0, 0);
            } else {
                Commit();
            }
            continue;
        }
        if (IsDialogKeyDown(msg, window_, VK_TAB)) {
            CycleFocus(stops, std::size(stops), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(window_, kRefreshTimerId);
    DestroyWindow(window_);
    window_ = nullptr;
    for (HFONT* font : {&font_, &headingFont_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    return result_;
}

}  // namespace polish
