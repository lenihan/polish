#include "hook/SettingsWindow.h"

#include <utility>

#include "util/UiFont.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishSettingsWindow";
constexpr int kFirstCheckId = 3000;

// Logical (96 DPI) layout constants.
constexpr int kClientWidth = 520;
constexpr int kMargin = 16;
constexpr int kCheckHeight = 22;
constexpr int kDescriptionIndent = 24;
constexpr int kRowGap = 14;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

}  // namespace

SettingsWindow::SettingsWindow(HINSTANCE instance, IsEnabledFn isEnabled, SetEnabledFn setEnabled)
    : instance_(instance), isEnabled_(std::move(isEnabled)), setEnabled_(std::move(setEnabled)) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcThunk;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    RegisterClassExW(&windowClass);
}

SettingsWindow::~SettingsWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
    }
}

LRESULT CALLBACK SettingsWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDCANCEL) {  // Esc, via IsDialogMessage
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED) {
                const size_t index = static_cast<size_t>(id - kFirstCheckId);
                if (index < rows_.size()) {
                    const Row& row = rows_[index];
                    const bool enabled = SendMessageW(row.check, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    setEnabled_(row.feature, enabled);
                }
            }
            return 0;
        }

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void SettingsWindow::CreateControls() {
    const UINT dpi = GetDpiForWindow(window_);
    if (font_ != nullptr) {
        DeleteObject(font_);
    }
    font_ = MakeUiFont(dpi);

    const int margin = Scale(kMargin, dpi);
    const int clientWidth = Scale(kClientWidth, dpi);
    const int indent = Scale(kDescriptionIndent, dpi);
    const int checkHeight = Scale(kCheckHeight, dpi);
    const int descriptionWidth = clientWidth - 2 * margin - indent;

    // Descriptions wrap, so each row's height is measured with the same
    // font the control will draw in.
    HDC dc = GetDC(window_);
    HGDIOBJ previousFont = SelectObject(dc, font_);

    int y = margin;
    int id = kFirstCheckId;
    for (const FeatureInfo& info : AllFeatures()) {
        Row row;
        row.feature = info.id;
        row.check = CreateWindowExW(0, L"BUTTON", info.name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    margin, y, clientWidth - 2 * margin, checkHeight, window_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id++)), instance_, nullptr);
        y += checkHeight;

        RECT measure{0, 0, descriptionWidth, 0};
        DrawTextW(dc, info.description, -1, &measure, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int descriptionHeight = measure.bottom - measure.top;
        row.description = CreateWindowExW(0, L"STATIC", info.description, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                          margin + indent, y, descriptionWidth, descriptionHeight, window_, nullptr,
                                          instance_, nullptr);
        y += descriptionHeight + Scale(kRowGap, dpi);

        SendMessageW(row.check, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(row.description, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        rows_.push_back(row);
    }
    SelectObject(dc, previousFont);
    ReleaseDC(window_, dc);

    // Size the window so the client area is exactly the content.
    RECT frame{0, 0, clientWidth, y - Scale(kRowGap, dpi) + margin};
    AdjustWindowRectExForDpi(&frame, WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    SetWindowPos(window_, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void SettingsWindow::Refresh() {
    for (const Row& row : rows_) {
        SendMessageW(row.check, BM_SETCHECK, isEnabled_(row.feature) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void SettingsWindow::Show() {
    if (window_ == nullptr) {
        window_ = CreateWindowExW(0, kWindowClassName, L"Polish Settings", WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            return;
        }
        CreateControls();

        RECT rect{};
        GetWindowRect(window_, &rect);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int x =
            monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - (rect.right - rect.left)) / 2;
        const int y =
            monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - (rect.bottom - rect.top)) / 2;
        SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Refresh();
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    if (!rows_.empty()) {
        SetFocus(rows_.front().check);
    }
}

bool SettingsWindow::PreTranslate(MSG& msg) {
    if (window_ == nullptr || !IsWindowVisible(window_)) {
        return false;
    }
    return IsDialogMessageW(window_, &msg) != FALSE;
}

}  // namespace polish
