#include "hook/AltTabListWindow.h"

#include <windowsx.h>

#include <algorithm>

#include "util/DarkMode.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishAltTabListWindow";

// Logical (96 DPI) px -- scaled fresh at every layout/paint via Scale(),
// never cached, same convention GroupChromeWindow/GroupPickerWindow use.
constexpr int kPanelWidth = 360;
constexpr int kPanelPaddingX = 8;
constexpr int kPanelPaddingY = 8;
constexpr int kRowHeight = 40;
constexpr int kRowPaddingX = 10;
constexpr int kIconSize = 24;
constexpr int kIconTextGap = 10;
constexpr int kPanelCornerRadius = 10;
constexpr int kRowCornerRadius = 6;

// The whole window (background AND row content) is rendered at this one
// constant alpha -- a "frosted, see-through-but-legible" panel doesn't
// need per-pixel alpha the way AltTabHighlightBorder's gradient ring
// does, so this reuses AltTabDimOverlay's simpler flat-alpha technique
// instead of the DIB+GDI+ pipeline.
constexpr BYTE kPanelAlpha = 235;

constexpr COLORREF kBackgroundColor = RGB(32, 32, 32);
constexpr COLORREF kTextColor = RGB(240, 240, 240);
constexpr COLORREF kHighlightTextColor = RGB(255, 255, 255);

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

}  // namespace

AltTabListWindow::AltTabListWindow(HINSTANCE instance) : instance_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }

    // WS_EX_NOACTIVATE (never steals focus) + WS_EX_TOPMOST, like the dim
    // overlays and highlight border -- but deliberately NOT
    // WS_EX_TRANSPARENT, since this window needs to receive its own mouse
    // clicks for row activation (see the class comment on isOwnUI/
    // ContainsPoint).
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kWindowClassName, L"", WS_POPUP, 0, 0,
                               0, 0, nullptr, nullptr, instance_, this);
    if (window_ != nullptr) {
        SetLayeredWindowAttributes(window_, 0, kPanelAlpha, LWA_ALPHA);
    }
}

AltTabListWindow::~AltTabListWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK AltTabListWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    AltTabListWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<AltTabListWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<AltTabListWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AltTabListWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            // Paint always fully repaints the client area itself (see
            // Paint) -- the default erase would just be a wasted extra
            // fill, same reasoning GroupChromeWindow already documents
            // for its own tab strip.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const int width = clientRect.right - clientRect.left;
            const int height = clientRect.bottom - clientRect.top;
            if (width > 0 && height > 0) {
                // Double-buffered for the same reason as GroupChromeWindow's
                // tab strip: many separate GDI calls straight to the live
                // screen HDC is a classic flicker source.
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
                HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);
                Paint(memDC, clientRect);
                BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBitmap);
                DeleteObject(memBitmap);
                DeleteDC(memDC);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const UINT dpi = GetDpiForWindow(hwnd);
            const std::vector<RECT> rowRects = ComputeRowRects(dpi);
            for (size_t i = 0; i < rowRects.size(); ++i) {
                RECT r = rowRects[i];  // PtInRect takes a non-const RECT*
                if (PtInRect(&r, pt)) {
                    if (onRowActivated_) {
                        onRowActivated_(i);
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_DPICHANGED: {
            // Standard MSDN pattern (same as GroupChromeWindow): resize to
            // the OS-suggested rect, then just repaint -- row layout is
            // recomputed fresh from the current DPI on every paint, no
            // separate relayout step needed.
            const auto* suggestedRect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggestedRect->left, suggestedRect->top,
                         suggestedRect->right - suggestedRect->left, suggestedRect->bottom - suggestedRect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

std::vector<RECT> AltTabListWindow::ComputeRowRects(UINT dpi) const {
    std::vector<RECT> rects;
    rects.reserve(rows_.size());
    const int paddingX = Scale(kPanelPaddingX, dpi);
    const int paddingY = Scale(kPanelPaddingY, dpi);
    const int rowHeight = Scale(kRowHeight, dpi);
    const int width = Scale(kPanelWidth, dpi) - 2 * paddingX;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const int top = paddingY + static_cast<int>(i) * rowHeight;
        rects.push_back(RECT{paddingX, top, paddingX + width, top + rowHeight});
    }
    return rects;
}

void AltTabListWindow::Paint(HDC hdc, const RECT& clientRect) const {
    const UINT dpi = GetDpiForWindow(window_);

    HBRUSH backgroundBrush = CreateSolidBrush(kBackgroundColor);
    FillRect(hdc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    SetBkMode(hdc, TRANSPARENT);
    const std::vector<RECT> rowRects = ComputeRowRects(dpi);
    const int iconSize = Scale(kIconSize, dpi);
    const int rowPaddingX = Scale(kRowPaddingX, dpi);
    const int iconTextGap = Scale(kIconTextGap, dpi);
    const int rowCornerRadius = Scale(kRowCornerRadius, dpi);
    const COLORREF accentColor = GetAccentColor();

    for (size_t i = 0; i < rowRects.size() && i < rows_.size(); ++i) {
        const RECT& rowRect = rowRects[i];
        const bool highlighted = (i == highlightIndex_);
        if (highlighted) {
            HBRUSH accentBrush = CreateSolidBrush(accentColor);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldBrush = SelectObject(hdc, accentBrush);
            HGDIOBJ oldPen = SelectObject(hdc, nullPen);
            RoundRect(hdc, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, rowCornerRadius,
                      rowCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(accentBrush);
        }

        int textLeft = rowRect.left + rowPaddingX;
        const AltTabListRow& row = rows_[i];
        if (row.icon != nullptr) {
            const int iconTop = rowRect.top + (rowRect.bottom - rowRect.top - iconSize) / 2;
            DrawIconEx(hdc, textLeft, iconTop, row.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            textLeft += iconSize + iconTextGap;
        }

        RECT textRect{textLeft, rowRect.top, rowRect.right - rowPaddingX, rowRect.bottom};
        SetTextColor(hdc, highlighted ? kHighlightTextColor : kTextColor);
        DrawTextW(hdc, row.title.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void AltTabListWindow::Reposition(HWND monitorAnchor, UINT dpi) {
    const int paddingY = Scale(kPanelPaddingY, dpi);
    const int rowHeight = Scale(kRowHeight, dpi);
    const int width = Scale(kPanelWidth, dpi);
    const int height = 2 * paddingY + static_cast<int>(rows_.size()) * rowHeight;

    HMONITOR monitor = MonitorFromWindow(monitorAnchor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT workArea{0, 0, width, height};
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        workArea = monitorInfo.rcWork;
    }
    const int x = workArea.left + (workArea.right - workArea.left - width) / 2;
    const int y = workArea.top + (workArea.bottom - workArea.top - height) / 2;

    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void AltTabListWindow::Show(const std::vector<AltTabListRow>& rows, size_t highlightIndex, HWND monitorAnchor) {
    if (window_ == nullptr) {
        return;
    }
    rows_ = rows;
    highlightIndex_ = rows_.empty() ? 0 : std::min(highlightIndex, rows_.size() - 1);

    const UINT dpi = GetDpiForWindow(monitorAnchor);
    Reposition(monitorAnchor, dpi);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void AltTabListWindow::SetHighlight(size_t index) {
    if (window_ == nullptr) {
        return;
    }
    // Re-assert topmost on every call, even if the index itself didn't
    // change -- among windows marked HWND_TOPMOST, whichever gets that
    // status *most recently* ends up frontmost (same rule this app's own
    // dim-overlay/highlight-border promotion already has to account for).
    // AltTabHighlightBorder::ShowAroundTarget re-asserts its own topmost
    // status every single cycle; this is the cheap path Show() itself
    // doesn't run through (Show already reasserts via Reposition), so
    // without this the border would visibly jump in front of the panel
    // on every cycle that takes this path -- confirmed as a real,
    // human-reported bug ("the highlight was sometimes on top of the
    // panel"), and this cheap path is the *common* case once the
    // candidate list holds a stable order across a session.
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (rows_.empty() || index >= rows_.size() || index == highlightIndex_) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    const std::vector<RECT> rowRects = ComputeRowRects(dpi);
    // Narrow invalidate -- just the two rows that actually change look,
    // not the whole panel. This codebase has hit the "unconditional full
    // repaint causes visible flashing" bug shape more than once already
    // (the tab strip's own hover highlight, a member-title-change
    // repaint); no reason to reintroduce it here.
    const size_t oldIndex = highlightIndex_;
    highlightIndex_ = index;
    if (oldIndex < rowRects.size()) {
        RECT r = rowRects[oldIndex];
        InvalidateRect(window_, &r, FALSE);
    }
    if (index < rowRects.size()) {
        RECT r = rowRects[index];
        InvalidateRect(window_, &r, FALSE);
    }
}

void AltTabListWindow::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

bool AltTabListWindow::IsVisible() const { return window_ != nullptr && IsWindowVisible(window_); }

bool AltTabListWindow::ContainsPoint(POINT screenPt) const {
    if (!IsVisible()) {
        return false;
    }
    RECT windowRect;
    if (!GetWindowRect(window_, &windowRect)) {
        return false;
    }
    return PtInRect(&windowRect, screenPt) != FALSE;
}

}  // namespace polish
