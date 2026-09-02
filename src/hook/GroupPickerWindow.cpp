#include "hook/GroupPickerWindow.h"

#include <commctrl.h>

#include <algorithm>
#include <format>

#include "util/Logging.h"
#include "windowtracking/WindowFilters.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupPickerWindow";
constexpr int kCreateButtonId = 1001;
constexpr int kCancelButtonId = 1002;
constexpr int kTabModeRadioId = 1003;
constexpr int kTileModeRadioId = 1004;

// Logical (96 DPI) layout constants -- scaled by the window's actual DPI
// in LayoutControls/before CreateWindowExW.
constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 620;
constexpr int kMargin = 12;
constexpr int kButtonHeight = 28;
constexpr int kButtonWidth = 110;
constexpr int kModeRowHeight = 24;
constexpr int kRadioWidth = 90;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

BOOL CALLBACK EnumPickerCandidatesProc(HWND hwnd, LPARAM lParam) {
    if (IsCandidateWindow(hwnd) && !IsElevatedWindow(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

}  // namespace

GroupPickerWindow::GroupPickerWindow(HINSTANCE instance) : instance_(instance) {
    static bool commonControlsInitialized = false;
    if (!commonControlsInitialized) {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icc);
        commonControlsInitialized = true;
    }

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
                if (LOWORD(wParam) == kCreateButtonId) {
                    Commit();
                } else if (LOWORD(wParam) == kCancelButtonId) {
                    result_.reset();
                    done_ = true;
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

void GroupPickerWindow::CreateControls(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);

    listView_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
                                 0, 0, 0, 0, hwnd, nullptr, instance_, nullptr);
    ListView_SetExtendedListViewStyle(listView_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<LPWSTR>(L"Window");
    column.cx = Scale(kWindowWidth - 2 * kMargin, dpi);
    ListView_InsertColumn(listView_, 0, &column);

    // WS_GROUP on the first radio starts a new keyboard-navigation/
    // auto-exclusion group so checking one unchecks the other (standard
    // Win32 radio-button grouping via tab order, not a container
    // control) -- Tab is the default (matches GroupMode's own default).
    tabModeRadio_ = CreateWindowExW(0, L"BUTTON", L"Tab",
                                     WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabModeRadioId)), instance_,
                                     nullptr);
    tileModeRadio_ = CreateWindowExW(0, L"BUTTON", L"Tile", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 0, 0, 0,
                                      0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTileModeRadioId)),
                                      instance_, nullptr);
    SendMessageW(initialMode_ == GroupMode::Tile ? tileModeRadio_ : tabModeRadio_, BM_SETCHECK, BST_CHECKED, 0);

    createButton_ = CreateWindowExW(0, L"BUTTON", editing_ ? L"Update Group" : L"Create Group",
                                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCreateButtonId)), instance_,
                                     nullptr);
    cancelButton_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                                     0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)),
                                     instance_, nullptr);

    HFONT dialogFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(listView_, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    SendMessageW(tabModeRadio_, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    SendMessageW(tileModeRadio_, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    SendMessageW(createButton_, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    SendMessageW(cancelButton_, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);

    LayoutControls();
}

void GroupPickerWindow::LayoutControls() {
    if (listView_ == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_ != nullptr ? window_ : listView_);
    RECT client{};
    GetClientRect(GetParent(listView_), &client);

    const int margin = Scale(kMargin, dpi);
    const int buttonHeight = Scale(kButtonHeight, dpi);
    const int buttonWidth = Scale(kButtonWidth, dpi);
    const int modeRowHeight = Scale(kModeRowHeight, dpi);
    const int radioWidth = Scale(kRadioWidth, dpi);

    const int buttonsTop = client.bottom - margin - buttonHeight;
    const int modeRowTop = buttonsTop - margin - modeRowHeight;
    const int listBottom = modeRowTop - margin;
    MoveWindow(listView_, margin, margin, client.right - 2 * margin, listBottom - margin, TRUE);

    MoveWindow(tabModeRadio_, margin, modeRowTop, radioWidth, modeRowHeight, TRUE);
    MoveWindow(tileModeRadio_, margin + radioWidth, modeRowTop, radioWidth, modeRowHeight, TRUE);

    MoveWindow(cancelButton_, client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(createButton_, client.right - 2 * margin - 2 * buttonWidth, buttonsTop, buttonWidth, buttonHeight,
               TRUE);
}

void GroupPickerWindow::PopulateList() {
    candidates_.clear();
    EnumWindows(EnumPickerCandidatesProc, reinterpret_cast<LPARAM>(&candidates_));

    // Existing group members are always kept in the list even if they'd
    // normally be filtered out (e.g. currently minimized) -- editing
    // membership should never silently drop a member just because of a
    // transient state at edit time.
    for (HWND hwnd : initialSelection_) {
        if (IsWindow(hwnd) && std::find(candidates_.begin(), candidates_.end(), hwnd) == candidates_.end()) {
            candidates_.push_back(hwnd);
        }
    }

    ListView_DeleteAllItems(listView_);
    for (size_t i = 0; i < candidates_.size(); ++i) {
        wchar_t title[256] = L"";
        GetWindowTextW(candidates_[i], title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        if (title[0] == L'\0') {
            continue;
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = title;
        ListView_InsertItem(listView_, &item);
        if (std::find(initialSelection_.begin(), initialSelection_.end(), candidates_[i]) !=
            initialSelection_.end()) {
            ListView_SetCheckState(listView_, static_cast<int>(i), TRUE);
        }
    }
    ListView_SetColumnWidth(listView_, 0, LVSCW_AUTOSIZE_USEHEADER);
}

void GroupPickerWindow::Commit() {
    std::vector<HWND> selected;
    const int itemCount = ListView_GetItemCount(listView_);
    for (int i = 0; i < itemCount; ++i) {
        if (ListView_GetCheckState(listView_, i) != 0) {
            selected.push_back(candidates_[static_cast<size_t>(i)]);
        }
    }
    const GroupMode mode =
        (SendMessageW(tileModeRadio_, BM_GETCHECK, 0, 0) == BST_CHECKED) ? GroupMode::Tile : GroupMode::Tab;
    LogDebug(std::format(L"[Polish] GroupPicker: confirmed with {} of {} candidate(s) selected, mode={}",
                          selected.size(), candidates_.size(), mode == GroupMode::Tile ? L"Tile" : L"Tab"));
    result_ = GroupPickerResult{std::move(selected), mode};
    done_ = true;
}

std::optional<GroupPickerResult> GroupPickerWindow::ShowModal(HWND owner, const std::vector<HWND>& initialSelection,
                                                                GroupMode initialMode, bool editing) {
    initialSelection_ = initialSelection;
    initialMode_ = initialMode;
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

    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName,
                               editing_ ? L"Edit Group — select windows" : L"New Group — select windows",
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

    DestroyWindow(window_);
    window_ = nullptr;
    listView_ = nullptr;
    tabModeRadio_ = nullptr;
    tileModeRadio_ = nullptr;
    createButton_ = nullptr;
    cancelButton_ = nullptr;
    return result_;
}

}  // namespace polish
