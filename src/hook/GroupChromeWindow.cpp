#include "hook/GroupChromeWindow.h"

#include <windowsx.h>

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupChromeWindow";
constexpr int kTabStripHeight = 36;   // logical (96 DPI) px
constexpr int kTabMinWidth = 120;     // logical px
constexpr int kTabMaxWidth = 220;     // logical px

// Local to this window's own context menu -- TrackPopupMenu's returned
// command isn't routed through WM_COMMAND, so these don't need to be
// unique app-wide.
constexpr UINT kContextMenuSwitchMode = 1;
constexpr UINT kContextMenuEditWindows = 2;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

}  // namespace

GroupChromeWindow::GroupChromeWindow(HINSTANCE instance) : instance_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
}

GroupChromeWindow::~GroupChromeWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK GroupChromeWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GroupChromeWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<GroupChromeWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GroupChromeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT GroupChromeWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            PaintTabStrip(hdc, clientRect);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (size_t i = 0; i < tabRects.size(); ++i) {
                RECT r = tabRects[i];  // PtInRect takes a non-const RECT*
                if (PtInRect(&r, pt)) {
                    // Activation is deferred to WM_LBUTTONUP (see
                    // there) so a click that turns into a drag ends up
                    // activating wherever the tab was dropped, not
                    // wherever it started.
                    draggingIndex_ = i;
                    SetCapture(hwnd);
                    break;
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!draggingIndex_.has_value()) {
                return 0;
            }
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (size_t i = 0; i < tabRects.size(); ++i) {
                RECT r = tabRects[i];
                if (PtInRect(&r, pt) && i != *draggingIndex_) {
                    if (onTabReordered_) {
                        onTabReordered_(*draggingIndex_, i);
                    }
                    // The dragged tab is now at index i -- reorder
                    // fires live, once per crossing, not just once on
                    // drop (see SetOnTabReordered's comment).
                    draggingIndex_ = i;
                    break;
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (draggingIndex_.has_value()) {
                // Captured before ReleaseCapture(), not after --
                // ReleaseCapture() synchronously re-enters this window
                // via WM_CAPTURECHANGED (below), which resets
                // draggingIndex_ itself; dereferencing it afterward
                // would read an already-empty optional.
                const size_t activatedIndex = *draggingIndex_;
                ReleaseCapture();
                if (onTabClicked_) {
                    onTabClicked_(activatedIndex);
                }
                draggingIndex_.reset();
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Mouse capture was taken by something else mid-drag (e.g.
            // another window stole focus) -- abandon the drag rather
            // than leaving draggingIndex_ stuck set, which would make
            // the next unrelated WM_MOUSEMOVE misbehave.
            draggingIndex_.reset();
            return 0;

        case WM_CONTEXTMENU: {
            POINT pt;
            if (lParam == -1) {
                // Triggered via keyboard (Shift+F10/VK_APPS), not a
                // real click position -- MSDN's documented sentinel.
                RECT windowRect{};
                GetWindowRect(hwnd, &windowRect);
                pt = POINT{(windowRect.left + windowRect.right) / 2, (windowRect.top + windowRect.bottom) / 2};
            } else {
                // Already screen coordinates for WM_CONTEXTMENU
                // (unlike WM_LBUTTONDOWN's client coordinates) -- no
                // conversion needed before TrackPopupMenu.
                pt = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            }
            ShowContextMenu(pt.x, pt.y);
            return 0;
        }

        case WM_WINDOWPOSCHANGING: {
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            if ((pos->flags & SWP_NOMOVE) == 0 && onMoved_) {
                // The window's outer (non-client) rect and its client
                // area aren't the same rect -- there's a title bar and
                // borders between them. That offset doesn't change
                // during a pure move, so it's measured once from the
                // window's current (still pre-move, at this point in
                // the message) state and re-applied to the proposed
                // WINDOWPOS position -- matches exactly what
                // ContentRectInScreenCoords() computes at rest, instead
                // of naively assuming the client area starts right at
                // the outer rect's edge (which ignored the title bar
                // entirely and misplaced every member during a drag).
                RECT outerRect;
                GetWindowRect(hwnd, &outerRect);
                POINT clientTopLeft{0, 0};
                POINT clientBottomRight{0, 0};
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                clientBottomRight = {clientRect.right, clientRect.bottom};
                ClientToScreen(hwnd, &clientTopLeft);
                ClientToScreen(hwnd, &clientBottomRight);

                const int offsetLeft = clientTopLeft.x - outerRect.left;
                const int offsetTop = clientTopLeft.y - outerRect.top;
                const int offsetRight = outerRect.right - clientBottomRight.x;
                const int offsetBottom = outerRect.bottom - clientBottomRight.y;

                const int width = (pos->flags & SWP_NOSIZE) ? (outerRect.right - outerRect.left) : pos->cx;
                const int height = (pos->flags & SWP_NOSIZE) ? (outerRect.bottom - outerRect.top) : pos->cy;

                const int newClientLeft = pos->x + offsetLeft;
                const int newClientTop = pos->y + offsetTop;
                const int newClientRight = pos->x + width - offsetRight;
                const int newClientBottom = pos->y + height - offsetBottom;

                const UINT dpi = GetDpiForWindow(hwnd);
                const int tabHeight = Scale(kTabStripHeight, dpi);
                const RECT newContentRect{newClientLeft, newClientTop + tabHeight, newClientRight,
                                           newClientBottom};
                onMoved_(newContentRect);
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_DESTROY:
            // Not the app's main message window -- no PostQuitMessage
            // here. Cleanup of any group-level state (GroupManager
            // membership, etc.) when a chrome window closes is M6 scope,
            // not this milestone's.
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

std::vector<RECT> GroupChromeWindow::ComputeTabRects(const RECT& clientRect) const {
    std::vector<RECT> rects;
    if (memberTitles_.empty() || mode_ == GroupMode::Tile) {
        return rects;  // Tile mode has no clickable tabs -- see Show()'s comment
    }
    const UINT dpi = GetDpiForWindow(window_);
    const int tabHeight = Scale(kTabStripHeight, dpi);
    const int tabMinWidth = Scale(kTabMinWidth, dpi);
    const int tabMaxWidth = Scale(kTabMaxWidth, dpi);

    const int availableWidth = clientRect.right - clientRect.left;
    int tabWidth = availableWidth / static_cast<int>(memberTitles_.size());
    tabWidth = std::max(tabMinWidth, std::min(tabWidth, tabMaxWidth));

    int x = clientRect.left;
    for (size_t i = 0; i < memberTitles_.size(); ++i) {
        if (x >= clientRect.right) {
            break;
        }
        rects.push_back(
            RECT{x, clientRect.top, std::min(x + tabWidth, static_cast<int>(clientRect.right)), clientRect.top + tabHeight});
        x += tabWidth;
    }
    return rects;
}

void GroupChromeWindow::PaintTabStrip(HDC hdc, const RECT& clientRect) {
    const UINT dpi = GetDpiForWindow(window_);
    const int tabHeight = Scale(kTabStripHeight, dpi);

    RECT stripRect{clientRect.left, clientRect.top, clientRect.right, clientRect.top + tabHeight};
    HBRUSH stripBrush = CreateSolidBrush(RGB(0xE8, 0xE8, 0xE8));
    FillRect(hdc, &stripRect, stripBrush);
    DeleteObject(stripBrush);

    RECT contentRect{clientRect.left, clientRect.top + tabHeight, clientRect.right, clientRect.bottom};
    HBRUSH contentBrush = CreateSolidBrush(RGB(0xFA, 0xFA, 0xFA));
    FillRect(hdc, &contentRect, contentBrush);
    DeleteObject(contentBrush);

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    if (mode_ == GroupMode::Tile) {
        // No individual tabs to draw/click in tile mode -- every member
        // is simultaneously visible in its own grid slot (GroupManager's
        // job), so the header is just a plain label.
        RECT labelRect = stripRect;
        InflateRect(&labelRect, -Scale(8, dpi), 0);
        SetTextColor(hdc, RGB(0x00, 0x00, 0x00));
        const std::wstring label =
            L"Group (" + std::to_wstring(memberTitles_.size()) + L" window(s), tiled)";
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, oldFont);
        return;
    }

    const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
    if (tabRects.empty()) {
        SelectObject(hdc, oldFont);
        return;
    }

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(0xC0, 0xC0, 0xC0));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);

    for (size_t i = 0; i < tabRects.size(); ++i) {
        const RECT& tabRect = tabRects[i];
        const bool active = (i == activeIndex_);

        HBRUSH tabBrush = CreateSolidBrush(active ? RGB(0x00, 0x78, 0xD7) : RGB(0xE8, 0xE8, 0xE8));
        HGDIOBJ oldBrush = SelectObject(hdc, tabBrush);
        Rectangle(hdc, tabRect.left, tabRect.top, tabRect.right, tabRect.bottom);
        SelectObject(hdc, oldBrush);
        DeleteObject(tabBrush);

        SetTextColor(hdc, active ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00));
        RECT textRect = tabRect;
        InflateRect(&textRect, -Scale(8, dpi), 0);
        DrawTextW(hdc, memberTitles_[i].c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);
    SelectObject(hdc, oldFont);
}

RECT GroupChromeWindow::ContentRectInScreenCoords() const {
    if (window_ == nullptr) {
        return RECT{};
    }
    RECT client;
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int tabHeight = Scale(kTabStripHeight, dpi);

    POINT topLeft{client.left, client.top + tabHeight};
    POINT bottomRight{client.right, client.bottom};
    ClientToScreen(window_, &topLeft);
    ClientToScreen(window_, &bottomRight);
    return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

void GroupChromeWindow::GrowContentAreaTo(SIZE minContentSize) {
    if (window_ == nullptr) {
        return;
    }
    const RECT currentContent = ContentRectInScreenCoords();
    const int currentWidth = currentContent.right - currentContent.left;
    const int currentHeight = currentContent.bottom - currentContent.top;
    if (minContentSize.cx <= currentWidth && minContentSize.cy <= currentHeight) {
        return;
    }

    // The gap between the outer window rect and the content area (title
    // bar, borders, and the tab strip itself) doesn't change with size,
    // so it's measured once and added back on top of whatever content
    // size is actually needed -- same offset-measurement approach as
    // the WM_WINDOWPOSCHANGING handler above, for the same reason (the
    // outer rect and the client/content rect are not the same rect).
    RECT windowRect{};
    GetWindowRect(window_, &windowRect);
    const int overheadWidth = (windowRect.right - windowRect.left) - currentWidth;
    const int overheadHeight = (windowRect.bottom - windowRect.top) - currentHeight;

    const int newWidth = std::max(static_cast<int>(minContentSize.cx), currentWidth) + overheadWidth;
    const int newHeight = std::max(static_cast<int>(minContentSize.cy), currentHeight) + overheadHeight;

    // SWP_NOMOVE -- top-left stays put; only WM_WINDOWPOSCHANGING's
    // onMoved_ notification is for actual moves (SWP_NOMOVE here means
    // it won't fire), the caller re-applies layout explicitly right
    // after calling this, so there's no risk of a feedback loop through
    // that callback.
    SetWindowPos(window_, nullptr, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void GroupChromeWindow::SetActiveIndex(size_t index) {
    activeIndex_ = index;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupChromeWindow::SetMemberTitles(const std::vector<std::wstring>& titles) {
    memberTitles_ = titles;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupChromeWindow::SetMode(GroupMode mode) {
    mode_ = mode;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupChromeWindow::ShowContextMenu(int screenX, int screenY) {
    if (window_ == nullptr) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kContextMenuSwitchMode,
                mode_ == GroupMode::Tab ? L"Switch to Tile" : L"Switch to Tab");
    AppendMenuW(menu, MF_STRING, kContextMenuEditWindows, L"Edit windows...");

    // The SetForegroundWindow/PostMessage(WM_NULL) pairing around
    // TrackPopupMenu is a documented Win32 requirement (MSDN), not
    // extra ceremony -- without it the menu can fail to close correctly
    // if this window doesn't already have focus when the menu opens.
    SetForegroundWindow(window_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenX, screenY, 0, window_, nullptr));
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == kContextMenuSwitchMode && onModeToggleRequested_) {
        onModeToggleRequested_();
    } else if (cmd == kContextMenuEditWindows && onEditWindowsRequested_) {
        onEditWindowsRequested_();
    }
}

void GroupChromeWindow::Show(const std::vector<std::wstring>& memberTitles, GroupMode mode) {
    memberTitles_ = memberTitles;
    mode_ = mode;

    if (window_ == nullptr) {
        window_ = CreateWindowExW(0, kWindowClassName, L"Group", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                   CW_USEDEFAULT, 800, 600, nullptr, nullptr, instance_, this);
    }
    if (window_ == nullptr) {
        return;
    }

    InvalidateRect(window_, nullptr, TRUE);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
}

}  // namespace polish
