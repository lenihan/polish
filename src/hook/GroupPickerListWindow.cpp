#include "hook/GroupPickerListWindow.h"

#include <windowsx.h>

#include <algorithm>
#include <iterator>

#include "util/DarkMode.h"
#include "util/WindowIcon.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupPickerListWindow";

// Logical (96 DPI) px -- scaled fresh at every layout/paint via Scale(),
// never cached, same convention every other custom-painted window in
// this codebase uses.
constexpr int kRowHeight = 40;
constexpr int kPaddingX = 14;
constexpr int kCheckboxSize = 18;
constexpr int kCheckboxTextGap = 12;
constexpr int kIconSize = 20;
constexpr int kIconTextGap = 10;
constexpr int kGripZoneWidth = 26;
constexpr int kGripLineWidth = 14;
constexpr int kRowCornerRadius = 6;
constexpr int kCheckboxCornerRadius = 4;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

std::wstring GetWindowTitle(HWND hwnd) {
    wchar_t buffer[256];
    const int length = GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return std::wstring(buffer, static_cast<size_t>(std::max(0, length)));
}

}  // namespace

GroupPickerListWindow::GroupPickerListWindow(HINSTANCE instance) : instance_(instance) {}

GroupPickerListWindow::~GroupPickerListWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

HWND GroupPickerListWindow::Create(HWND parent) {
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
    window_ = CreateWindowExW(0, kWindowClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, parent,
                               nullptr, instance_, this);
    return window_;
}

LRESULT CALLBACK GroupPickerListWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GroupPickerListWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<GroupPickerListWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GroupPickerListWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT GroupPickerListWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            // Paint always fully repaints the client area itself -- same
            // reasoning GroupChromeWindow/AltTabListWindow already
            // document for their own owner-painted surfaces.
            return 1;

        case WM_NCDESTROY:
            // Fires whether this child is destroyed directly (this
            // class's own destructor) or implicitly, cascaded from its
            // parent's DestroyWindow (GroupPickerWindow::ShowModal's own
            // teardown, which reuses this object -- and this same HWND
            // slot -- across multiple ShowModal calls). Either way,
            // window_ must go back to nullptr so a later Create() call
            // doesn't leave a stale handle behind, and so the
            // destructor's own DestroyWindow call becomes a correctly-
            // skipped no-op instead of a double-destroy.
            window_ = nullptr;
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const int width = clientRect.right - clientRect.left;
            const int height = clientRect.bottom - clientRect.top;
            if (width > 0 && height > 0) {
                SelectClipRgn(hdc, nullptr);
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

        case WM_SIZE:
            UpdateScrollInfo();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pt.y += scrollOffset_;
            const UINT dpi = GetDpiForWindow(hwnd);
            const RowLayout layout = ComputeLayout(dpi);
            const int paddingX = Scale(kPaddingX, dpi);
            const int gripZoneWidth = Scale(kGripZoneWidth, dpi);
            for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
                RECT r = layout.rowRects[i];
                if (!PtInRect(&r, pt)) {
                    continue;
                }
                if (rows_[i].checked) {
                    RECT gripRect{r.right - paddingX - gripZoneWidth, r.top, r.right - paddingX, r.bottom};
                    if (PtInRect(&gripRect, pt)) {
                        BeginDrag(i);
                        return 0;
                    }
                }
                ToggleChecked(i);
                return 0;
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);

            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (dragging_) {
                UpdateDrag(pt);
                return 0;
            }
            pt.y += scrollOffset_;
            const UINT dpi = GetDpiForWindow(hwnd);
            const std::vector<RECT> rowRects = ComputeLayout(dpi).rowRects;
            std::optional<size_t> newHover;
            for (size_t i = 0; i < rowRects.size(); ++i) {
                RECT r = rowRects[i];
                if (PtInRect(&r, pt)) {
                    newHover = i;
                    break;
                }
            }
            SetHoveredIndex(newHover);
            return 0;
        }

        case WM_LBUTTONUP:
            EndDrag();
            return 0;

        case WM_CAPTURECHANGED:
            dragging_ = false;
            return 0;

        case WM_MOUSELEAVE:
            SetHoveredIndex(std::nullopt);
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            const int step = Scale(kRowHeight, dpi);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const int contentHeight = ComputeLayout(dpi).contentHeight;
            const int maxScroll = std::max(0, contentHeight - static_cast<int>(clientRect.bottom - clientRect.top));
            scrollOffset_ = std::clamp(scrollOffset_ - (delta / WHEEL_DELTA) * step, 0, maxScroll);
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_POS;
            si.nPos = scrollOffset_;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            const UINT dpi = GetDpiForWindow(hwnd);
            int newPos = si.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP:
                    newPos -= Scale(kRowHeight, dpi) / 4;
                    break;
                case SB_LINEDOWN:
                    newPos += Scale(kRowHeight, dpi) / 4;
                    break;
                case SB_PAGEUP:
                    newPos -= static_cast<int>(si.nPage);
                    break;
                case SB_PAGEDOWN:
                    newPos += static_cast<int>(si.nPage);
                    break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                    newPos = si.nTrackPos;
                    break;
                default:
                    break;
            }
            const int maxScroll = std::max(0, si.nMax - static_cast<int>(si.nPage) + 1);
            scrollOffset_ = std::clamp(newPos, 0, maxScroll);
            si.fMask = SIF_POS;
            si.nPos = scrollOffset_;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

GroupPickerListWindow::RowLayout GroupPickerListWindow::ComputeLayout(UINT dpi) const {
    RowLayout layout;
    RECT clientRect{};
    if (window_ != nullptr) {
        GetClientRect(window_, &clientRect);
    }
    const int width = clientRect.right - clientRect.left;
    const int rowHeight = Scale(kRowHeight, dpi);
    layout.rowRects.reserve(rows_.size());
    int top = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        layout.rowRects.push_back(RECT{0, top, width, top + rowHeight});
        top += rowHeight;
    }
    layout.contentHeight = top;
    return layout;
}

void GroupPickerListWindow::Paint(HDC hdc, const RECT& clientRect) const {
    const UINT dpi = GetDpiForWindow(window_);
    const bool dark = IsDarkModeEnabled();
    const COLORREF backgroundColor = dark ? RGB(0x2B, 0x2B, 0x2B) : GetSysColor(COLOR_WINDOW);
    const COLORREF textColor = dark ? RGB(0xE8, 0xE8, 0xE8) : GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF hoverColor = dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xEC, 0xEC, 0xEC);
    const COLORREF borderColor = dark ? RGB(0x55, 0x55, 0x55) : RGB(0xC0, 0xC0, 0xC0);
    const COLORREF checkboxOutline = dark ? RGB(0x90, 0x90, 0x90) : RGB(0x70, 0x70, 0x70);
    const COLORREF gripColor = dark ? RGB(0x80, 0x80, 0x80) : RGB(0xA0, 0xA0, 0xA0);
    const COLORREF accentColor = GetAccentColor();

    HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
    FillRect(hdc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    // Everything below (until the viewport origin is restored) is drawn
    // in ComputeLayout's "natural" unshifted coordinates -- see
    // AltTabListWindow::Paint's own comment on this same technique.
    POINT priorViewportOrg{};
    SetViewportOrgEx(hdc, 0, -scrollOffset_, &priorViewportOrg);

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT textFont = CreateFontIndirectW(&metrics.lfMessageFont);
    HGDIOBJ oldFont = SelectObject(hdc, textFont);
    SetBkMode(hdc, TRANSPARENT);

    const RowLayout layout = ComputeLayout(dpi);
    const int paddingX = Scale(kPaddingX, dpi);
    const int checkboxSize = Scale(kCheckboxSize, dpi);
    const int checkboxTextGap = Scale(kCheckboxTextGap, dpi);
    const int iconSize = Scale(kIconSize, dpi);
    const int iconTextGap = Scale(kIconTextGap, dpi);
    const int gripZoneWidth = Scale(kGripZoneWidth, dpi);
    const int gripLineWidth = Scale(kGripLineWidth, dpi);
    const int rowCornerRadius = Scale(kRowCornerRadius, dpi);
    const int checkboxCornerRadius = Scale(kCheckboxCornerRadius, dpi);

    for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
        const RECT& rowRect = layout.rowRects[i];
        const GroupPickerRow& row = rows_[i];
        const bool hovered = hoveredIndex_.has_value() && *hoveredIndex_ == i;
        const int rowMidY = (rowRect.top + rowRect.bottom) / 2;

        if (hovered) {
            HBRUSH fillBrush = CreateSolidBrush(hoverColor);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldBrush = SelectObject(hdc, fillBrush);
            HGDIOBJ oldPen = SelectObject(hdc, nullPen);
            RoundRect(hdc, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, rowCornerRadius, rowCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(fillBrush);
        }

        int x = rowRect.left + paddingX;

        RECT checkboxRect{x, rowMidY - checkboxSize / 2, x + checkboxSize, rowMidY + checkboxSize / 2};
        if (row.checked) {
            HBRUSH checkBrush = CreateSolidBrush(accentColor);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldBrush = SelectObject(hdc, checkBrush);
            HGDIOBJ oldPen = SelectObject(hdc, nullPen);
            RoundRect(hdc, checkboxRect.left, checkboxRect.top, checkboxRect.right, checkboxRect.bottom,
                      checkboxCornerRadius, checkboxCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(checkBrush);

            HPEN checkPen = CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), RGB(0xFF, 0xFF, 0xFF));
            HGDIOBJ oldCheckPen = SelectObject(hdc, checkPen);
            const int cx = checkboxRect.left;
            const int cy = checkboxRect.top;
            MoveToEx(hdc, cx + checkboxSize * 2 / 10, cy + checkboxSize * 5 / 10, nullptr);
            LineTo(hdc, cx + checkboxSize * 4 / 10, cy + checkboxSize * 7 / 10);
            LineTo(hdc, cx + checkboxSize * 8 / 10, cy + checkboxSize * 3 / 10);
            SelectObject(hdc, oldCheckPen);
            DeleteObject(checkPen);
        } else {
            HPEN outlinePen = CreatePen(PS_SOLID, 1, checkboxOutline);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            HGDIOBJ oldPen = SelectObject(hdc, outlinePen);
            RoundRect(hdc, checkboxRect.left, checkboxRect.top, checkboxRect.right, checkboxRect.bottom,
                      checkboxCornerRadius, checkboxCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(outlinePen);
        }
        x += checkboxSize + checkboxTextGap;

        if (row.icon != nullptr) {
            const int iconTop = rowMidY - iconSize / 2;
            DrawIconEx(hdc, x, iconTop, row.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            x += iconSize + iconTextGap;
        }

        RECT textRect{x, rowRect.top, rowRect.right - paddingX - gripZoneWidth, rowRect.bottom};
        SetTextColor(hdc, textColor);
        DrawTextW(hdc, row.title.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (row.checked) {
            const int gripLeft = rowRect.right - paddingX - gripZoneWidth + (gripZoneWidth - gripLineWidth) / 2;
            HPEN gripPen = CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), gripColor);
            HGDIOBJ oldPen = SelectObject(hdc, gripPen);
            for (int line = -1; line <= 1; ++line) {
                const int y = rowMidY + line * Scale(5, dpi);
                MoveToEx(hdc, gripLeft, y, nullptr);
                LineTo(hdc, gripLeft + gripLineWidth, y);
            }
            SelectObject(hdc, oldPen);
            DeleteObject(gripPen);
        }
    }

    SetViewportOrgEx(hdc, priorViewportOrg.x, priorViewportOrg.y, nullptr);
    SelectObject(hdc, oldFont);
    DeleteObject(textFont);

    HPEN borderPen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldBorderPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBorderBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, clientRect.left, clientRect.top, clientRect.right, clientRect.bottom);
    SelectObject(hdc, oldBorderPen);
    SelectObject(hdc, oldBorderBrush);
    DeleteObject(borderPen);
}

void GroupPickerListWindow::UpdateScrollInfo() {
    if (window_ == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    RECT clientRect;
    GetClientRect(window_, &clientRect);
    const int viewportHeight = clientRect.bottom - clientRect.top;
    const int contentHeight = ComputeLayout(dpi).contentHeight;
    const int maxScroll = std::max(0, contentHeight - viewportHeight);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, contentHeight - 1);
    si.nPage = static_cast<UINT>(std::max(0, viewportHeight));
    si.nPos = scrollOffset_;
    SetScrollInfo(window_, SB_VERT, &si, TRUE);
}

size_t GroupPickerListWindow::CheckedCount() const {
    size_t count = 0;
    for (const GroupPickerRow& row : rows_) {
        if (!row.checked) {
            break;
        }
        ++count;
    }
    return count;
}

void GroupPickerListWindow::SetHoveredIndex(std::optional<size_t> index) {
    if (index.has_value() && *index >= rows_.size()) {
        index = std::nullopt;
    }
    if (index == hoveredIndex_) {
        return;
    }
    hoveredIndex_ = index;
    // A plain full-invalidate here (not the two-row narrow-invalidate
    // AltTabListWindow uses) -- this list is small and infrequently
    // hovered-over compared to Alt+Tab's own mouse-tracked panel, so the
    // simpler code isn't worth trading away for that micro-optimization.
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupPickerListWindow::ToggleChecked(size_t index) {
    if (index >= rows_.size()) {
        return;
    }
    GroupPickerRow row = rows_[index];
    rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(index));
    if (!row.checked) {
        row.checked = true;
        const size_t checkedCount = CheckedCount();
        rows_.insert(rows_.begin() + static_cast<ptrdiff_t>(checkedCount), row);
    } else {
        row.checked = false;
        rows_.push_back(row);
    }
    hoveredIndex_.reset();  // row positions all shifted; simplest safe reset
    if (window_ != nullptr) {
        UpdateScrollInfo();
        InvalidateRect(window_, nullptr, TRUE);
    }
    NotifyChanged();
}

void GroupPickerListWindow::BeginDrag(size_t index) {
    dragging_ = true;
    dragIndex_ = index;
    if (window_ != nullptr) {
        SetCapture(window_);
    }
}

void GroupPickerListWindow::UpdateDrag(POINT clientPt) {
    if (!dragging_ || window_ == nullptr) {
        return;
    }
    clientPt.y += scrollOffset_;
    const UINT dpi = GetDpiForWindow(window_);
    const RowLayout layout = ComputeLayout(dpi);
    const size_t checkedCount = CheckedCount();
    // Bounded to [0, checkedCount) -- a drag can never cross into the
    // unchecked suffix (see class comment on the interaction model).
    for (size_t i = 0; i < checkedCount && i < layout.rowRects.size(); ++i) {
        if (i == dragIndex_) {
            continue;
        }
        RECT r = layout.rowRects[i];
        if (PtInRect(&r, clientPt)) {
            GroupPickerRow moved = rows_[dragIndex_];
            rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(dragIndex_));
            rows_.insert(rows_.begin() + static_cast<ptrdiff_t>(i), moved);
            dragIndex_ = i;
            InvalidateRect(window_, nullptr, TRUE);
            NotifyChanged();
            break;
        }
    }
}

void GroupPickerListWindow::EndDrag() {
    dragging_ = false;
    if (window_ != nullptr && GetCapture() == window_) {
        ReleaseCapture();
    }
}

void GroupPickerListWindow::NotifyChanged() {
    if (onChanged_) {
        onChanged_();
    }
}

void GroupPickerListWindow::SetWindows(const std::vector<HWND>& candidates, const std::vector<HWND>& initialChecked) {
    rows_.clear();
    rows_.reserve(candidates.size() + initialChecked.size());
    for (HWND hwnd : initialChecked) {
        rows_.push_back(GroupPickerRow{hwnd, GetWindowTitle(hwnd), GetWindowIconHandle(hwnd), /*checked=*/true});
    }
    for (HWND hwnd : candidates) {
        const bool alreadyChecked =
            std::any_of(rows_.begin(), rows_.end(), [hwnd](const GroupPickerRow& row) { return row.hwnd == hwnd; });
        if (alreadyChecked) {
            continue;
        }
        rows_.push_back(GroupPickerRow{hwnd, GetWindowTitle(hwnd), GetWindowIconHandle(hwnd), /*checked=*/false});
    }
    hoveredIndex_.reset();
    scrollOffset_ = 0;
    if (window_ != nullptr) {
        UpdateScrollInfo();
        InvalidateRect(window_, nullptr, TRUE);
    }
    NotifyChanged();
}

std::vector<HWND> GroupPickerListWindow::CheckedWindows() const {
    std::vector<HWND> result;
    for (const GroupPickerRow& row : rows_) {
        if (!row.checked) {
            break;
        }
        result.push_back(row.hwnd);
    }
    return result;
}

}  // namespace polish
