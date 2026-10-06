#include "hook/StackStripWindow.h"

#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

#include "resource.h"
#include "util/DarkMode.h"
#include "util/Logging.h"
#include "util/UiFont.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishStackStrip";
constexpr wchar_t kTaskbarClassName[] = L"PolishStackTaskbar";

int Scale(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// Tab metrics in DIPs. The floor is room for an icon and a little of a title;
// below that a tab is not worth drawing.
constexpr int kTabMaxWidthDip = 220;
constexpr int kTabFloorWidthDip = 48;
constexpr int kTabGapDip = 4;
// The strip's start is a drag grip, not just padding: with enough tabs they fill
// the whole strip, and a strip that cannot be grabbed cannot be moved.
constexpr int kLeadingPadDip = 22;
constexpr int kGripDotDip = 2;
constexpr int kCrossPadDip = 4;
constexpr int kRowHeightDip = 32;
constexpr int kTabCornerDip = 6;
constexpr int kMinimizeButtonDip = 40;
constexpr int kTitleTextLeftDip = 12;
constexpr int kIconDip = 16;
constexpr int kIconLeftDip = 8;
constexpr int kTextGapDip = 6;
constexpr int kTextRightDip = 8;
constexpr int kAccentBarDip = 3;
// How far a dragged tab must travel before it counts as a drag rather than a
// click with a shaky hand, and how far clear of the strip before it counts as
// leaving it. The system drag size covers the first; the second is generous
// on purpose, since tearing a window out is a deliberate act.
constexpr int kTearThresholdDip = 48;

const COLORREF kAccent = RGB(0x4C, 0xA0, 0xE8);

}  // namespace

int StackStripWindow::ThicknessPx(StackAlignment alignment, UINT dpi) {
    return Scale(alignment == StackAlignment::Horizontal ? kHorizontalThicknessDip : kVerticalThicknessDip, dpi);
}

StackStripWindow::StackStripWindow(HINSTANCE instance) : instance_(instance) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // Double-click on the title bar opens the rename/edit UI.
        windowClass.style = CS_DBLCLKS;
        windowClass.lpszClassName = kWindowClassName;
        // No background brush: every pixel is painted, and a brush here
        // would flash its colour on each resize.
        registered = RegisterClassExW(&windowClass) != 0;
    }
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWindowClassName, L"Polish stack",
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);

    // The taskbar stand-in. See TaskbarHandle().
    static bool taskbarRegistered = false;
    if (!taskbarRegistered) {
        WNDCLASSEXW taskbarClass{};
        taskbarClass.cbSize = sizeof(taskbarClass);
        taskbarClass.lpfnWndProc = TaskbarProcThunk;
        taskbarClass.hInstance = instance;
        taskbarClass.lpszClassName = kTaskbarClassName;
        taskbarRegistered = RegisterClassExW(&taskbarClass) != 0;
    }
    // WS_EX_APPWINDOW forces a taskbar button; the window is one pixel,
    // offscreen, and never painted. (Not WS_EX_NOACTIVATE: the shell
    // activates it when its button is clicked, and that activation is exactly
    // the signal the owner acts on.)
    taskbarWindow_ = CreateWindowExW(WS_EX_APPWINDOW, kTaskbarClassName, L"Stack", WS_POPUP, -32000, -32000, 1, 1,
                                      nullptr, nullptr, instance, this);
    if (taskbarWindow_ != nullptr) {
        HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_POLISH_TRAY));
        if (icon != nullptr) {
            SendMessageW(taskbarWindow_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(taskbarWindow_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
        // An AppUserModelID of its own, so the shell gives this stack its own
        // button rather than grouping every stack (and Polish) under one.
        const std::wstring appId = std::format(L"Polish.Stack.{:X}", reinterpret_cast<uintptr_t>(this));
        Microsoft::WRL::ComPtr<IPropertyStore> store;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(taskbarWindow_, IID_PPV_ARGS(&store))) && store) {
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_LPWSTR;
            const size_t bytes = (appId.size() + 1) * sizeof(wchar_t);
            value.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
            if (value.pwszVal != nullptr) {
                memcpy(value.pwszVal, appId.c_str(), bytes);
                store->SetValue(PKEY_AppUserModel_ID, value);
                store->Commit();
            }
            PropVariantClear(&value);
        }
        ShowWindow(taskbarWindow_, SW_SHOWNOACTIVATE);
    }
}

StackStripWindow::~StackStripWindow() {
    if (taskbarWindow_ != nullptr) {
        DestroyWindow(taskbarWindow_);
    }
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void StackStripWindow::SetTitle(const std::wstring& title) {
    title_ = title;
    if (window_ != nullptr) {
        SetWindowTextW(window_, title.c_str());
        InvalidateRect(window_, nullptr, FALSE);
    }
    if (taskbarWindow_ != nullptr) {
        SetWindowTextW(taskbarWindow_, title.c_str());
    }
}

RECT StackStripWindow::TitleBarRect(const RECT& client, UINT dpi) const {
    return RECT{client.left, client.top, client.right, std::min(client.bottom, client.top + Scale(kTitleBarDip, dpi))};
}

RECT StackStripWindow::TabAreaRect(const RECT& client, UINT dpi) const {
    RECT area = client;
    area.top = TitleBarRect(client, dpi).bottom;
    return area;
}

RECT StackStripWindow::MinimizeButtonRect(const RECT& client, UINT dpi) const {
    const RECT title = TitleBarRect(client, dpi);
    return RECT{title.right - Scale(kMinimizeButtonDip, dpi), title.top, title.right, title.bottom};
}

LRESULT CALLBACK StackStripWindow::TaskbarProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    StackStripWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<StackStripWindow*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StackStripWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    switch (message) {
        case WM_CLOSE:
            // "Close window" on the taskbar button: close the stack, which
            // dissolves it and leaves its windows where they are. Never
            // destroyed here -- the owner decides, and destroys it later.
            if (self != nullptr && self->onTaskbarCloseRequested_) {
                self->onTaskbarCloseRequested_();
            }
            return 0;
        case WM_SYSCOMMAND:
            // This window is never really shown, so minimizing or restoring
            // it means nothing in itself. The shell sends these when its
            // button is clicked while the window is already in front, and
            // what the user means is the stack as a whole: the owner toggles
            // it. Swallowed either way, so the stand-in is never minimized.
            if ((wParam & 0xFFF0) == SC_MINIMIZE || (wParam & 0xFFF0) == SC_RESTORE) {
                if (self != nullptr && self->onTaskbarToggle_) {
                    self->onTaskbarToggle_();
                }
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void StackStripWindow::SetTabs(std::vector<StripTab> tabs, size_t activeIndex) {
    tabs_ = std::move(tabs);
    activeIndex_ = activeIndex;
    if (hoveredTab_.has_value() && *hoveredTab_ >= tabs_.size()) {
        hoveredTab_.reset();
    }
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void StackStripWindow::SetActiveIndex(size_t activeIndex) {
    if (activeIndex_ != activeIndex) {
        activeIndex_ = activeIndex;
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

void StackStripWindow::SetAlignment(StackAlignment alignment) {
    if (alignment_ != alignment) {
        alignment_ = alignment;
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

void StackStripWindow::PlaceAt(const RECT& screenRect) {
    if (window_ == nullptr) {
        return;
    }
    // HWND_TOPMOST every time, not just at creation: this is what keeps the
    // strip over a stack that has just been raised, and Polish is a
    // background process, so HWND_TOP would silently do nothing.
    SetWindowPos(window_, HWND_TOPMOST, screenRect.left, screenRect.top, screenRect.right - screenRect.left,
                 screenRect.bottom - screenRect.top, SWP_NOACTIVATE);
}

void StackStripWindow::Show() {
    if (window_ != nullptr && !IsWindowVisible(window_)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
}

void StackStripWindow::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

bool StackStripWindow::IsShown() const {
    return window_ != nullptr && IsWindowVisible(window_) != FALSE;
}

RECT StackStripWindow::ScreenRect() const {
    RECT rect{};
    if (window_ != nullptr) {
        GetWindowRect(window_, &rect);
    }
    return rect;
}

bool StackStripWindow::ContainsScreenPoint(POINT screenPt) const {
    const RECT rect = ScreenRect();
    return IsShown() && PtInRect(&rect, screenPt) != FALSE;
}

void StackStripWindow::SetDropCaret(std::optional<size_t> insertionIndex) {
    if (dropCaret_ != insertionIndex) {
        dropCaret_ = insertionIndex;
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

size_t StackStripWindow::InsertionIndexAtScreenPoint(POINT screenPt) const {
    POINT client = screenPt;
    ScreenToClient(window_, &client);
    return InsertionIndexAt(CurrentTabRects(), client, alignment_);
}

TabMetrics StackStripWindow::MetricsFor(UINT dpi) const {
    TabMetrics metrics;
    metrics.maxWidth = Scale(kTabMaxWidthDip, dpi);
    metrics.floorWidth = Scale(kTabFloorWidthDip, dpi);
    metrics.gap = Scale(kTabGapDip, dpi);
    metrics.leadingPad = Scale(kLeadingPadDip, dpi);
    metrics.crossPad = Scale(kCrossPadDip, dpi);
    metrics.rowHeight = Scale(kRowHeightDip, dpi);
    return metrics;
}

std::vector<RECT> StackStripWindow::CurrentTabRects() const {
    RECT client{};
    GetClientRect(window_, &client);
    return TabRects(TabAreaRect(client, GetDpiForWindow(window_)), static_cast<int>(tabs_.size()), alignment_, MetricsFor(GetDpiForWindow(window_)));
}

void StackStripWindow::EndDrag() {
    pressedTab_.reset();
    tabDragging_ = false;
    tearing_ = false;
    stripDragging_ = false;
    minimizePressed_ = false;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void StackStripWindow::Paint(HDC hdc, const RECT& client) {
    const UINT dpi = GetDpiForWindow(window_);
    const bool dark = IsDarkModeEnabled();
    const COLORREF background = dark ? RGB(0x1B, 0x1B, 0x1B) : RGB(0xE9, 0xE9, 0xE9);
    const COLORREF activeFill = dark ? RGB(0x34, 0x34, 0x34) : RGB(0xFF, 0xFF, 0xFF);
    const COLORREF hoverFill = dark ? RGB(0x28, 0x28, 0x28) : RGB(0xF3, 0xF3, 0xF3);
    const COLORREF border = dark ? RGB(0x4A, 0x4A, 0x4A) : RGB(0xB8, 0xB8, 0xB8);
    const COLORREF text = dark ? RGB(0xF0, 0xF0, 0xF0) : RGB(0x1A, 0x1A, 0x1A);
    const COLORREF dimText = dark ? RGB(0x8A, 0x8A, 0x8A) : RGB(0x80, 0x80, 0x80);

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(hdc, &client, backgroundBrush);
    DeleteObject(backgroundBrush);

    // A one-pixel frame, so the strip reads as an object against whatever
    // happens to be behind it.
    HBRUSH borderBrush = CreateSolidBrush(border);
    FrameRect(hdc, &client, borderBrush);
    DeleteObject(borderBrush);

    const RECT area = TabAreaRect(client, dpi);
    const std::vector<RECT> rects = TabRects(area, static_cast<int>(tabs_.size()), alignment_, MetricsFor(dpi));

    HFONT font = MakeUiFont(dpi);
    HFONT boldFont = MakeUiFontWithWeight(dpi, FW_SEMIBOLD);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    // The title bar: the stack's name, and a minimize button for the whole
    // stack. It is a drag handle too (dragging it moves the stack), like any
    // title bar, which is also what makes a stack read as one window rather
    // than a loose strip of tabs.
    {
        const RECT titleBar = TitleBarRect(client, dpi);
        const COLORREF titleFill = dark ? RGB(0x12, 0x12, 0x12) : RGB(0xDD, 0xDD, 0xDD);
        HBRUSH titleBrush = CreateSolidBrush(titleFill);
        FillRect(hdc, &titleBar, titleBrush);
        DeleteObject(titleBrush);
        const RECT rule{titleBar.left, titleBar.bottom - 1, titleBar.right, titleBar.bottom};
        HBRUSH ruleBrush = CreateSolidBrush(border);
        FillRect(hdc, &rule, ruleBrush);
        DeleteObject(ruleBrush);

        const RECT minimize = MinimizeButtonRect(client, dpi);
        if (minimizeHovered_ || minimizePressed_) {
            HBRUSH hover = CreateSolidBrush(dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xC8, 0xC8, 0xC8));
            RECT button = minimize;
            button.bottom -= 1;
            FillRect(hdc, &button, hover);
            DeleteObject(hover);
        }
        // The glyph: a short horizontal line, the same shape every caption
        // minimize button draws.
        const int glyphW = Scale(10, dpi);
        const int glyphY = (minimize.top + minimize.bottom) / 2;
        HPEN glyphPen = CreatePen(PS_SOLID, std::max(1, Scale(1, dpi)), text);
        HGDIOBJ oldPen = SelectObject(hdc, glyphPen);
        MoveToEx(hdc, (minimize.left + minimize.right - glyphW) / 2, glyphY, nullptr);
        LineTo(hdc, (minimize.left + minimize.right + glyphW) / 2, glyphY);
        SelectObject(hdc, oldPen);
        DeleteObject(glyphPen);

        RECT nameRect{titleBar.left + Scale(kTitleTextLeftDip, dpi), titleBar.top, minimize.left - Scale(8, dpi),
                      titleBar.bottom - 1};
        if (nameRect.right > nameRect.left) {
            SelectObject(hdc, boldFont);
            SetTextColor(hdc, text);
            DrawTextW(hdc, title_.c_str(), -1, &nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, font);
        }
    }

    // The drag grip: a small block of dots in the leading pad, dragging
    // anywhere in it (or in any other gap) moves the whole stack.
    {
        const int dot = std::max(2, Scale(kGripDotDip, dpi));
        const int spacing = dot * 2;
        const int pad = Scale(kLeadingPadDip, dpi);
        // Two columns of three dots, centred in the pad: along the strip for
        // Horizontal, across it (a row of three, two deep) for Vertical.
        const bool horizontal = alignment_ == StackAlignment::Horizontal;
        const int cols = horizontal ? 2 : 3;
        const int rowsCount = horizontal ? 3 : 2;
        const int blockW = cols * dot + (cols - 1) * (spacing - dot);
        const int blockH = rowsCount * dot + (rowsCount - 1) * (spacing - dot);
        const int originX = horizontal ? area.left + (pad - blockW) / 2 : area.left + ((area.right - area.left) - blockW) / 2;
        const int originY = horizontal ? area.top + ((area.bottom - area.top) - blockH) / 2 : area.top + (pad - blockH) / 2;
        HBRUSH dotBrush = CreateSolidBrush(dimText);
        for (int r = 0; r < rowsCount; ++r) {
            for (int c = 0; c < cols; ++c) {
                const RECT d{originX + c * spacing, originY + r * spacing, originX + c * spacing + dot,
                             originY + r * spacing + dot};
                FillRect(hdc, &d, dotBrush);
            }
        }
        DeleteObject(dotBrush);
    }

    const int corner = Scale(kTabCornerDip, dpi);
    const int iconSize = Scale(kIconDip, dpi);
    for (size_t i = 0; i < rects.size(); ++i) {
        const RECT& r = rects[i];
        const bool active = i == activeIndex_;
        const bool dragged = tabDragging_ && pressedTab_ == i;
        const bool hovered = hoveredTab_ == i;

        if (active || hovered || dragged) {
            HBRUSH fill = CreateSolidBrush(active ? activeFill : hoverFill);
            HGDIOBJ oldBrush = SelectObject(hdc, fill);
            HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
            RoundRect(hdc, r.left, r.top, r.right + 1, r.bottom + 1, corner * 2, corner * 2);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(fill);
        }
        if (active) {
            // The accent bar marks the active tab even where the fill is close
            // to the strip's own colour.
            const int bar = Scale(kAccentBarDip, dpi);
            RECT accent = alignment_ == StackAlignment::Horizontal
                              ? RECT{r.left + corner, r.bottom - bar, r.right - corner, r.bottom}
                              : RECT{r.left, r.top + corner, r.left + bar, r.bottom - corner};
            HBRUSH accentBrush = CreateSolidBrush(kAccent);
            FillRect(hdc, &accent, accentBrush);
            DeleteObject(accentBrush);
        }

        int textLeft = r.left + Scale(kIconLeftDip, dpi);
        if (tabs_[i].icon != nullptr) {
            const int iconTop = r.top + ((r.bottom - r.top) - iconSize) / 2;
            DrawIconEx(hdc, textLeft, iconTop, tabs_[i].icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            textLeft += iconSize + Scale(kTextGapDip, dpi);
        }
        RECT textRect{textLeft, r.top, r.right - Scale(kTextRightDip, dpi), r.bottom};
        if (textRect.right > textRect.left) {
            SelectObject(hdc, active ? boldFont : font);
            SetTextColor(hdc, tabs_[i].minimized ? dimText : text);
            DrawTextW(hdc, tabs_[i].title.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    // The insertion caret for a window being dragged in.
    if (dropCaret_.has_value()) {
        const int thickness = std::max(2, Scale(2, dpi));
        const int gap = Scale(kTabGapDip, dpi);
        RECT caret{};
        if (rects.empty()) {
            caret = alignment_ == StackAlignment::Horizontal
                        ? RECT{Scale(kLeadingPadDip, dpi), area.top + 4, Scale(kLeadingPadDip, dpi) + thickness, area.bottom - 4}
                        : RECT{area.left + 4, area.top + Scale(kLeadingPadDip, dpi), area.right - 4, area.top + Scale(kLeadingPadDip, dpi) + thickness};
        } else if (alignment_ == StackAlignment::Horizontal) {
            const int x = *dropCaret_ < rects.size() ? rects[*dropCaret_].left - gap / 2 : rects.back().right + gap / 2;
            caret = RECT{x - thickness / 2, area.top + 3, x - thickness / 2 + thickness, area.bottom - 3};
        } else {
            const int y = *dropCaret_ < rects.size() ? rects[*dropCaret_].top - gap / 2 : rects.back().bottom + gap / 2;
            caret = RECT{area.left + 3, y - thickness / 2, area.right - 3, y - thickness / 2 + thickness};
        }
        HBRUSH caretBrush = CreateSolidBrush(kAccent);
        FillRect(hdc, &caret, caretBrush);
        DeleteObject(caretBrush);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
    DeleteObject(boldFont);
}

LRESULT CALLBACK StackStripWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    StackStripWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<StackStripWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StackStripWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->HandleMessage(hwnd, message, wParam, lParam);
}

LRESULT StackStripWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_MOUSEACTIVATE:
            // Never take activation: see the class comment.
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;  // Paint covers everything.

        case WM_DPICHANGED:
            // The owner re-derives the strip's rect from the stack rect on
            // every reflow, so the rect Windows suggests here is ignored: a
            // strip that resized itself would race the reflow that follows.
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (width > 0 && height > 0) {
                // Double-buffered: a tab drag repaints at mouse rate, and
                // painting straight to the window would flicker.
                HDC memory = CreateCompatibleDC(hdc);
                HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
                HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
                Paint(memory, client);
                BitBlt(hdc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
                SelectObject(memory, oldBitmap);
                DeleteObject(bitmap);
                DeleteDC(memory);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SETCURSOR:
            if (tearing_) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            pressScreenPt_ = screen;
            lastScreenPt_ = screen;
            RECT clientNow{};
            GetClientRect(hwnd, &clientNow);
            const UINT dpiNow = GetDpiForWindow(hwnd);
            const RECT minimizeRect = MinimizeButtonRect(clientNow, dpiNow);
            const RECT titleRect = TitleBarRect(clientNow, dpiNow);
            const auto tab = TabIndexAt(CurrentTabRects(), pt);
            if (PtInRect(&minimizeRect, pt)) {
                // A button: acted on at release, if still over it.
                minimizePressed_ = true;
            } else if (tab.has_value()) {
                pressedTab_ = tab;
            } else {
                // The title bar, the grip, or a gap: all of them move the stack.
                (void)titleRect;
                stripDragging_ = true;
            }
            SetCapture(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);

            if (!trackingMouseLeave_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                if (TrackMouseEvent(&track)) {
                    trackingMouseLeave_ = true;
                }
            }

            if (stripDragging_) {
                const int dx = screen.x - lastScreenPt_.x;
                const int dy = screen.y - lastScreenPt_.y;
                lastScreenPt_ = screen;
                if ((dx != 0 || dy != 0) && onStripDragged_) {
                    onStripDragged_(dx, dy);
                }
                return 0;
            }

            if (pressedTab_.has_value()) {
                const UINT dpi = GetDpiForWindow(hwnd);
                if (!tabDragging_) {
                    const int slopX = GetSystemMetricsForDpi(SM_CXDRAG, dpi);
                    const int slopY = GetSystemMetricsForDpi(SM_CYDRAG, dpi);
                    if (std::abs(screen.x - pressScreenPt_.x) > slopX || std::abs(screen.y - pressScreenPt_.y) > slopY) {
                        tabDragging_ = true;
                    }
                }
                if (tabDragging_) {
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    // One decision, made here, for both outcomes -- see
                    // DragTearsOut.
                    const bool nowTearing = DragTearsOut(client, pt, Scale(kTearThresholdDip, dpi));
                    if (nowTearing != tearing_) {
                        tearing_ = nowTearing;
                        SetCursor(LoadCursorW(nullptr, tearing_ ? IDC_SIZEALL : IDC_ARROW));
                    }
                    if (!tearing_) {
                        const size_t target =
                            ReorderTargetIndex(CurrentTabRects(), *pressedTab_, pt, alignment_);
                        if (target != *pressedTab_) {
                            const size_t from = *pressedTab_;
                            pressedTab_ = target;
                            if (onTabReordered_) {
                                onTabReordered_(from, target);
                            }
                        }
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            const auto hover = TabIndexAt(CurrentTabRects(), pt);
            RECT clientNow{};
            GetClientRect(hwnd, &clientNow);
            const RECT minimizeRect = MinimizeButtonRect(clientNow, GetDpiForWindow(hwnd));
            const bool overMinimize = PtInRect(&minimizeRect, pt) != FALSE;
            if (hover != hoveredTab_ || overMinimize != minimizeHovered_) {
                hoveredTab_ = hover;
                minimizeHovered_ = overMinimize;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        case WM_LBUTTONUP: {
            // Decided before the capture is released: releasing it re-enters
            // as WM_CAPTURECHANGED, which clears all of this.
            const std::optional<size_t> pressed = pressedTab_;
            const bool wasMinimizePress = minimizePressed_;
            const bool wasDragging = tabDragging_;
            const bool wasTearing = tearing_;
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &screen);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            const POINT releasePt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT clientUp{};
            GetClientRect(hwnd, &clientUp);
            const RECT minimizeUp = MinimizeButtonRect(clientUp, GetDpiForWindow(hwnd));
            const bool releasedOnMinimize = PtInRect(&minimizeUp, releasePt) != FALSE;
            // The callbacks may tear this stack down. The owner defers any
            // destruction of this window (to a posted message) for exactly
            // that reason, so nothing here touches `this` afterwards.
            if (wasMinimizePress && releasedOnMinimize) {
                if (onMinimizeAll_) {
                    onMinimizeAll_();
                }
            } else if (pressed.has_value()) {
                if (wasTearing) {
                    if (onTabTornOut_) {
                        onTabTornOut_(*pressed, screen);
                    }
                } else if (!wasDragging) {
                    if (onTabClicked_) {
                        onTabClicked_(*pressed);
                    }
                }
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // The common end of every drag, however it happened (button up,
            // or capture taken by something else mid-drag).
            EndDrag();
            return 0;

        case WM_RBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (onContextMenu_) {
                onContextMenu_(screen, TabIndexAt(CurrentTabRects(), pt));
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouseLeave_ = false;
            if (hoveredTab_.has_value() || minimizeHovered_) {
                hoveredTab_.reset();
                minimizeHovered_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDBLCLK: {
            // A double-click on the title bar (not its button) opens the
            // rename/edit UI. The first click of the pair already began a strip
            // drag, which the capture release ends.
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT clientNow{};
            GetClientRect(hwnd, &clientNow);
            const UINT dpiNow = GetDpiForWindow(hwnd);
            const RECT title = TitleBarRect(clientNow, dpiNow);
            const RECT minimizeRect = MinimizeButtonRect(clientNow, dpiNow);
            if (PtInRect(&title, pt) && !PtInRect(&minimizeRect, pt) && onTitleDoubleClicked_) {
                onTitleDoubleClicked_();
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}  // namespace polish
