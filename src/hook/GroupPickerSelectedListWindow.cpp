#include "hook/GroupPickerSelectedListWindow.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>

#include "util/DarkMode.h"
#include "util/WindowIcon.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupPickerSelectedListWindow";

// Logical (96 DPI) px -- same row-height/padding/icon/button values
// GroupPickerListWindow uses for its own rows, so a row looks identical
// whether it's currently in this pane or the "Available windows" one.
constexpr int kRowHeight = 40;
constexpr int kPaddingX = 14;
constexpr int kIconSize = 20;
constexpr int kIconTextGap = 10;
constexpr int kRowCornerRadius = 6;
constexpr int kButtonSize = 24;
constexpr int kButtonGap = 6;
constexpr int kButtonCornerRadius = 6;
constexpr int kGlyphMargin = 7;
constexpr int kGripZoneWidth = 26;
constexpr int kGripLineWidth = 14;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// See GroupPickerListWindow's identical helper -- distinguishes a
// selected row's fill from a merely-hovered one.
COLORREF BlendColor(COLORREF base, COLORREF tint, double amount) {
    const int r = static_cast<int>(GetRValue(base) * (1 - amount) + GetRValue(tint) * amount);
    const int g = static_cast<int>(GetGValue(base) * (1 - amount) + GetGValue(tint) * amount);
    const int b = static_cast<int>(GetBValue(base) * (1 - amount) + GetBValue(tint) * amount);
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
}

// See GroupPickerListWindow's identical helper -- powers the title
// tooltip UpdateTooltip shows only when a row's own text is actually
// clipped by DT_END_ELLIPSIS.
bool IsTitleTruncated(HWND window, const std::wstring& title, int availableWidth, UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    HDC hdc = GetDC(window);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SIZE size{};
    GetTextExtentPoint32W(hdc, title.c_str(), static_cast<int>(title.size()), &size);
    SelectObject(hdc, oldFont);
    ReleaseDC(window, hdc);
    DeleteObject(font);
    return size.cx > availableWidth;
}

std::wstring GetWindowTitle(HWND hwnd) {
    wchar_t buffer[256];
    const int length = GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return std::wstring(buffer, static_cast<size_t>(std::max(0, length)));
}

// Rightmost -- the grip sits at the row's far edge, the natural "grab
// here to drag" spot; Remove sits just to its left, its own distinct
// target so a near-missed drag attempt can never land on it by accident
// (see this class's own header comment on why that separation matters).
RECT ComputeGripRect(const RECT& rowRect, UINT dpi) {
    const int gripWidth = Scale(kGripZoneWidth, dpi);
    const int paddingX = Scale(kPaddingX, dpi);
    const int left = rowRect.right - paddingX - gripWidth;
    return RECT{left, rowRect.top, left + gripWidth, rowRect.bottom};
}

RECT ComputeRemoveButtonRect(const RECT& rowRect, UINT dpi) {
    const RECT grip = ComputeGripRect(rowRect, dpi);
    const int buttonSize = Scale(kButtonSize, dpi);
    const int gap = Scale(kButtonGap, dpi);
    const int top = rowRect.top + (rowRect.bottom - rowRect.top - buttonSize) / 2;
    const int left = grip.left - gap - buttonSize;
    return RECT{left, top, left + buttonSize, top + buttonSize};
}

}  // namespace

GroupPickerSelectedListWindow::GroupPickerSelectedListWindow(HINSTANCE instance) : instance_(instance) {
    static bool commonControlsInitialized = false;
    if (!commonControlsInitialized) {
        // ICC_TAB_CLASSES, not the more obviously-named ICC_*TOOLTIP*
        // flag -- comctl32 groups the tooltip common control in with tab
        // controls historically; see GroupPickerListWindow's own
        // constructor (and GroupChromeWindow's before it) for the same
        // call -- an independent copy per class, not shared state,
        // since calling this more than once is harmless.
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
        commonControlsInitialized = true;
    }
}

GroupPickerSelectedListWindow::~GroupPickerSelectedListWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

HWND GroupPickerSelectedListWindow::Create(HWND parent) {
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

    // One manually-tracked tool covering both Remove and the grip --
    // text and position are set fresh in UpdateTooltip on every hover
    // change, same shape GroupPickerListWindow's own single-button
    // tooltip uses (see GroupChromeWindow::UpdateTooltip for the
    // original TTF_TRACK-based technique this mirrors).
    tooltipWindow_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                      WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                                      CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    if (tooltipWindow_ != nullptr) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = window_;
        ti.uId = 1;
        ti.lpszText = const_cast<LPWSTR>(L"");
        SendMessageW(tooltipWindow_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }

    return window_;
}

LRESULT CALLBACK GroupPickerSelectedListWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                                 LPARAM lParam) {
    GroupPickerSelectedListWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<GroupPickerSelectedListWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GroupPickerSelectedListWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT GroupPickerSelectedListWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_NCDESTROY:
            // See GroupPickerListWindow's identical handler for why this
            // matters: this object (and this HWND slot) is reused across
            // multiple GroupPickerWindow::ShowModal calls.
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

        case WM_SETFOCUS:
            hasFocus_ = true;
            // See GroupPickerListWindow's identical handler for why an
            // otherwise-unselected list selects its first row here.
            if (!selectedIndex_.has_value() && !rows_.empty() && onRowSelected_) {
                onRowSelected_(rows_[0].hwnd);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_KILLFOCUS:
            hasFocus_ = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_KEYDOWN: {
            // Left is this list's half of the move gesture -- the
            // keyboard equivalent of clicking the row's own Remove
            // button. Right does nothing here: a Group member can only
            // move left, back into Open windows (see the sibling list's
            // own VK_RIGHT case for the other half).
            if (wParam == VK_LEFT) {
                if (selectedIndex_.has_value() && onRemoveRequested_) {
                    onRemoveRequested_(rows_[*selectedIndex_].hwnd);
                }
                return 0;
            }
            if (wParam != VK_UP && wParam != VK_DOWN) {
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            if (rows_.empty()) {
                return 0;
            }
            // Moves the selection only -- deliberately not the row's
            // position. Reordering is the grip's job (see BeginDrag);
            // there's no keyboard equivalent for it yet, and quietly
            // overloading the arrow keys with it would make an ordinary
            // "move the selection down" gesture silently rearrange the
            // group instead.
            size_t next = 0;
            if (selectedIndex_.has_value()) {
                const size_t current = *selectedIndex_;
                next = wParam == VK_DOWN ? std::min(current + 1, rows_.size() - 1)
                                          : (current == 0 ? 0 : current - 1);
            }
            if (onRowSelected_) {
                onRowSelected_(rows_[next].hwnd);
            }
            EnsureRowVisible(next);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // See GroupPickerListWindow's identical call: keep keyboard
            // focus with whichever panel the mouse last acted on.
            SetFocus(hwnd);
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pt.y += scrollOffset_;
            const UINT dpi = GetDpiForWindow(hwnd);
            const RowLayout layout = ComputeLayout(dpi);
            // Only the selected-or-hovered row(s) ever draw Remove/the
            // grip (see Paint) -- mirror that same set here, same shape
            // GroupPickerListWindow's own Add-button hit-test uses.
            for (std::optional<size_t> rowIndex : {selectedIndex_, hoveredIndex_}) {
                if (!rowIndex.has_value() || *rowIndex >= layout.rowRects.size() || *rowIndex >= rows_.size()) {
                    continue;
                }
                const RECT& r = layout.rowRects[*rowIndex];
                const RECT gripRect = ComputeGripRect(r, dpi);
                if (PtInRect(&gripRect, pt)) {
                    BeginDrag(*rowIndex);
                    return 0;
                }
                const RECT removeRect = ComputeRemoveButtonRect(r, dpi);
                if (PtInRect(&removeRect, pt)) {
                    if (onRemoveRequested_) {
                        onRemoveRequested_(rows_[*rowIndex].hwnd);
                    }
                    return 0;
                }
            }
            // Not on a visible Remove/grip -- select whichever row (if
            // any) was actually clicked (see SetOnRowSelected's own
            // comment for why this class doesn't decide selection
            // itself).
            for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
                if (PtInRect(&layout.rowRects[i], pt)) {
                    if (onRowSelected_) {
                        onRowSelected_(rows_[i].hwnd);
                    }
                    return 0;
                }
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
            UpdateTooltip();
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
            UpdateTooltip();
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

GroupPickerSelectedListWindow::RowLayout GroupPickerSelectedListWindow::ComputeLayout(UINT dpi) const {
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

void GroupPickerSelectedListWindow::Paint(HDC hdc, const RECT& clientRect) const {
    const UINT dpi = GetDpiForWindow(window_);
    const bool dark = IsDarkModeEnabled();
    const COLORREF backgroundColor = dark ? RGB(0x2B, 0x2B, 0x2B) : GetSysColor(COLOR_WINDOW);
    const COLORREF textColor = dark ? RGB(0xE8, 0xE8, 0xE8) : GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF hoverColor = dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xEC, 0xEC, 0xEC);
    const COLORREF borderColor = dark ? RGB(0x55, 0x55, 0x55) : RGB(0xC0, 0xC0, 0xC0);
    const COLORREF buttonBorderColor = dark ? RGB(0x70, 0x70, 0x70) : RGB(0x90, 0x90, 0x90);
    const COLORREF removeGlyphColor = dark ? RGB(0xF0, 0x8C, 0x8C) : RGB(0xC4, 0x3E, 0x3E);
    const COLORREF gripColor = dark ? RGB(0x80, 0x80, 0x80) : RGB(0xA0, 0xA0, 0xA0);
    const COLORREF placeholderColor = dark ? RGB(0x80, 0x80, 0x80) : RGB(0x90, 0x90, 0x90);
    // Distinct from hoverColor -- see GroupPickerListWindow's identical
    // reasoning for why selected and merely-hovered need to look different.
    const COLORREF selectedColor = BlendColor(backgroundColor, GetAccentColor(), 0.35);

    HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
    FillRect(hdc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT textFont = CreateFontIndirectW(&metrics.lfMessageFont);
    HGDIOBJ oldFont = SelectObject(hdc, textFont);
    SetBkMode(hdc, TRANSPARENT);

    if (rows_.empty()) {
        RECT placeholderRect = clientRect;
        placeholderRect.left += Scale(kPaddingX, dpi);
        placeholderRect.right -= Scale(kPaddingX, dpi);
        SetTextColor(hdc, placeholderColor);
        DrawTextW(hdc, L"No windows selected yet", -1, &placeholderRect,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        // Everything below (until the viewport origin is restored) is
        // drawn in ComputeLayout's "natural" unshifted coordinates --
        // see AltTabListWindow::Paint's own comment on this technique.
        POINT priorViewportOrg{};
        SetViewportOrgEx(hdc, 0, -scrollOffset_, &priorViewportOrg);

        const RowLayout layout = ComputeLayout(dpi);
        const int paddingX = Scale(kPaddingX, dpi);
        const int iconSize = Scale(kIconSize, dpi);
        const int iconTextGap = Scale(kIconTextGap, dpi);
        const int rowCornerRadius = Scale(kRowCornerRadius, dpi);
        const int buttonCornerRadius = Scale(kButtonCornerRadius, dpi);
        const int glyphMargin = Scale(kGlyphMargin, dpi);
        const int gripLineWidth = Scale(kGripLineWidth, dpi);

        for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
            const RECT& rowRect = layout.rowRects[i];
            const GroupPickerSelectedRow& row = rows_[i];
            const bool hovered = hoveredIndex_.has_value() && *hoveredIndex_ == i;
            const bool selected = selectedIndex_.has_value() && *selectedIndex_ == i;
            const bool showButtons = selected || hovered;
            const int rowMidY = (rowRect.top + rowRect.bottom) / 2;

            // The row's own background fill is a full-bleed rounded rect
            // covering the *entire* row width, buttons included --
            // selectedColor takes priority over hoverColor when a row is
            // somehow both (confirmed, human-reported: before this, a
            // selected row and a merely-hovered one were visually
            // identical) -- drawn before anything else, so every later
            // element (button outlines, glyphs, text) paints on top of
            // a guaranteed-correct, fully opaque surface with no gap a
            // button's own small bounding box could leave unpainted.
            const COLORREF rowFillColor = selected ? selectedColor : (hovered ? hoverColor : backgroundColor);
            HBRUSH rowFillBrush = CreateSolidBrush(rowFillColor);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldRowBrush = SelectObject(hdc, rowFillBrush);
            HGDIOBJ oldRowPen = SelectObject(hdc, nullPen);
            RoundRect(hdc, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, rowCornerRadius, rowCornerRadius);
            SelectObject(hdc, oldRowBrush);
            SelectObject(hdc, oldRowPen);
            DeleteObject(rowFillBrush);

            int x = rowRect.left + paddingX;
            if (row.icon != nullptr) {
                const int iconTop = rowMidY - iconSize / 2;
                DrawIconEx(hdc, x, iconTop, row.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
                x += iconSize + iconTextGap;
            }

            const RECT removeRect = ComputeRemoveButtonRect(rowRect, dpi);
            RECT textRect{x, rowRect.top, removeRect.left - paddingX, rowRect.bottom};
            SetTextColor(hdc, textColor);
            DrawTextW(hdc, row.title.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Remove: a left-pointing arrow -- move this window back
            // toward the Available windows list. Grip: three short
            // horizontal lines -- drag from anywhere in this zone to
            // reorder. Both only drawn (and only hit-testable, see
            // WM_LBUTTONDOWN) on the selected-or-hovered row, mirroring
            // GroupPickerListWindow's own Add button -- the reserved
            // textRect width above stays constant regardless, so text
            // never reflows as these appear/disappear on different rows.
            if (showButtons) {
                HPEN buttonPen = CreatePen(PS_SOLID, 1, buttonBorderColor);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                HGDIOBJ oldPen = SelectObject(hdc, buttonPen);
                RoundRect(hdc, removeRect.left, removeRect.top, removeRect.right, removeRect.bottom,
                          buttonCornerRadius, buttonCornerRadius);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(buttonPen);

                HPEN glyphPen = CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), removeGlyphColor);
                HGDIOBJ oldGlyphPen = SelectObject(hdc, glyphPen);
                const int gLeft = removeRect.left + glyphMargin;
                const int gRight = removeRect.right - glyphMargin;
                const int gMidY = (removeRect.top + removeRect.bottom) / 2;
                MoveToEx(hdc, gRight, gMidY, nullptr);
                LineTo(hdc, gLeft, gMidY);
                MoveToEx(hdc, gLeft + Scale(5, dpi), gMidY - Scale(5, dpi), nullptr);
                LineTo(hdc, gLeft, gMidY);
                LineTo(hdc, gLeft + Scale(5, dpi), gMidY + Scale(5, dpi));
                SelectObject(hdc, oldGlyphPen);
                DeleteObject(glyphPen);

                const RECT gripRect = ComputeGripRect(rowRect, dpi);
                const int gripLeft = gripRect.left + (gripRect.right - gripRect.left - gripLineWidth) / 2;
                HPEN gripPen = CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), gripColor);
                HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
                for (int line = -1; line <= 1; ++line) {
                    const int y = rowMidY + line * Scale(5, dpi);
                    MoveToEx(hdc, gripLeft, y, nullptr);
                    LineTo(hdc, gripLeft + gripLineWidth, y);
                }
                SelectObject(hdc, oldGripPen);
                DeleteObject(gripPen);
            }
        }

        SetViewportOrgEx(hdc, priorViewportOrg.x, priorViewportOrg.y, nullptr);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(textFont);

    // Accent border while focused -- see GroupPickerListWindow's
    // identical block.
    HPEN borderPen = hasFocus_ ? CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), GetAccentColor())
                                : CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldBorderPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBorderBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, clientRect.left, clientRect.top, clientRect.right, clientRect.bottom);
    SelectObject(hdc, oldBorderPen);
    SelectObject(hdc, oldBorderBrush);
    DeleteObject(borderPen);
}

// See GroupPickerListWindow::EnsureRowVisible -- identical logic against
// this class's own rows.
void GroupPickerSelectedListWindow::EnsureRowVisible(size_t index) {
    if (window_ == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    const RowLayout layout = ComputeLayout(dpi);
    if (index >= layout.rowRects.size()) {
        return;
    }
    RECT clientRect;
    GetClientRect(window_, &clientRect);
    const int viewportHeight = clientRect.bottom - clientRect.top;
    const RECT& row = layout.rowRects[index];

    int newOffset = scrollOffset_;
    if (row.top < scrollOffset_) {
        newOffset = row.top;
    } else if (row.bottom > scrollOffset_ + viewportHeight) {
        newOffset = row.bottom - viewportHeight;
    }
    const int maxScroll = std::max(0, layout.contentHeight - viewportHeight);
    newOffset = std::clamp(newOffset, 0, maxScroll);
    if (newOffset == scrollOffset_) {
        return;
    }
    scrollOffset_ = newOffset;

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = scrollOffset_;
    SetScrollInfo(window_, SB_VERT, &si, TRUE);
    InvalidateRect(window_, nullptr, TRUE);
}

void GroupPickerSelectedListWindow::UpdateScrollInfo() {
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

void GroupPickerSelectedListWindow::SetHoveredIndex(std::optional<size_t> index) {
    if (index.has_value() && *index >= rows_.size()) {
        index = std::nullopt;
    }
    if (index == hoveredIndex_) {
        return;
    }
    hoveredIndex_ = index;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupPickerSelectedListWindow::BeginDrag(size_t index) {
    // Pressing the grip selects that row too, same as a plain body click
    // -- otherwise dragging a row that was only *hovered* (not yet
    // selected) would leave whichever row was previously selected still
    // marked selected while a different one is actually being dragged.
    // Reuses the exact same callback a body click already fires;
    // GroupPickerWindow owns what "selected" means, this just reports
    // the gesture.
    if (onRowSelected_ && index < rows_.size()) {
        onRowSelected_(rows_[index].hwnd);
    }
    // Don't leave the tooltip floating over its pre-drag position for
    // the rest of the drag.
    if (tooltipWindow_ != nullptr) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = window_;
        ti.uId = 1;
        SendMessageW(tooltipWindow_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    }
    // Hover is meaningless once a drag starts (the mouse is captured, and
    // WM_MOUSEMOVE's dragging branch never recomputes it) -- left stale,
    // it kept showing Remove/grip on whichever row was hovered *before*
    // the drag began, even after UpdateDrag's swap moved a different
    // window into that slot. Confirmed, human-reported: two rows
    // appeared highlighted mid-drag. The real mouse position naturally
    // repopulates this on the next WM_MOUSEMOVE once dragging ends.
    hoveredIndex_.reset();
    dragging_ = true;
    dragIndex_ = index;
    if (window_ != nullptr) {
        SetCapture(window_);
    }
}

void GroupPickerSelectedListWindow::UpdateDrag(POINT clientPt) {
    if (!dragging_ || window_ == nullptr) {
        return;
    }
    clientPt.y += scrollOffset_;
    const UINT dpi = GetDpiForWindow(window_);
    const RowLayout layout = ComputeLayout(dpi);
    for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
        if (i == dragIndex_) {
            continue;
        }
        RECT r = layout.rowRects[i];
        if (PtInRect(&r, clientPt)) {
            GroupPickerSelectedRow moved = rows_[dragIndex_];
            rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(dragIndex_));
            rows_.insert(rows_.begin() + static_cast<ptrdiff_t>(i), moved);
            // The dragged row is always the selected one (BeginDrag
            // selects it before the drag starts), so carry
            // selectedIndex_ along with the move. Without this the
            // highlight -- and the Remove/grip buttons, gated on
            // selected||hovered, whose hover half WM_MOUSEMOVE
            // deliberately stops updating mid-drag -- stay pinned to
            // the old slot, which now holds a *different* window, so
            // the row actually being dragged visibly loses its
            // selection (confirmed, human-reported).
            if (selectedIndex_.has_value() && *selectedIndex_ == dragIndex_) {
                selectedIndex_ = i;
            }
            dragIndex_ = i;
            InvalidateRect(window_, nullptr, TRUE);
            break;
        }
    }
}

void GroupPickerSelectedListWindow::EndDrag() {
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    if (window_ != nullptr && GetCapture() == window_) {
        ReleaseCapture();
    }
    if (onReordered_) {
        std::vector<HWND> order;
        order.reserve(rows_.size());
        for (const GroupPickerSelectedRow& row : rows_) {
            order.push_back(row.hwnd);
        }
        onReordered_(std::move(order));
    }
}

void GroupPickerSelectedListWindow::SetSelected(const std::vector<HWND>& order) {
    rows_.clear();
    rows_.reserve(order.size());
    for (HWND hwnd : order) {
        rows_.push_back(GroupPickerSelectedRow{hwnd, GetWindowTitle(hwnd), GetWindowIconHandle(hwnd)});
    }
    hoveredIndex_.reset();
    // selectedIndex_ deliberately left untouched here -- GroupPickerWindow
    // always follows a SetSelected call with SetSelectedHwnd, recomputing
    // it fresh against the new rows_ (see that method and this class's
    // own header comment on why selection isn't decided in here).
    scrollOffset_ = 0;
    if (window_ != nullptr) {
        UpdateScrollInfo();
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupPickerSelectedListWindow::SetSelectedHwnd(std::optional<HWND> hwnd) {
    std::optional<size_t> newIndex;
    if (hwnd.has_value()) {
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].hwnd == *hwnd) {
                newIndex = i;
                break;
            }
        }
    }
    if (newIndex == selectedIndex_) {
        return;
    }
    selectedIndex_ = newIndex;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupPickerSelectedListWindow::UpdateTooltip() {
    if (tooltipWindow_ == nullptr) {
        return;
    }

    // Three tiers: Remove and the grip's own tooltips take priority;
    // otherwise, if the hovered row's title doesn't fit its column
    // (DT_END_ELLIPSIS truncated it), show the full title -- see
    // GroupPickerListWindow::UpdateTooltip's identical reasoning.
    const wchar_t* text = nullptr;
    std::wstring hoveredTitle;
    if (hoveredIndex_.has_value() && window_ != nullptr && *hoveredIndex_ < rows_.size()) {
        const UINT dpi = GetDpiForWindow(window_);
        const RowLayout layout = ComputeLayout(dpi);
        if (*hoveredIndex_ < layout.rowRects.size()) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            cursor.y += scrollOffset_;
            const RECT& r = layout.rowRects[*hoveredIndex_];
            RECT removeRect = ComputeRemoveButtonRect(r, dpi);
            RECT gripRect = ComputeGripRect(r, dpi);
            if (PtInRect(&removeRect, cursor)) {
                text = L"Remove from group";
            } else if (PtInRect(&gripRect, cursor)) {
                text = L"Drag to reorder";
            } else {
                const GroupPickerSelectedRow& row = rows_[*hoveredIndex_];
                int x = Scale(kPaddingX, dpi);
                if (row.icon != nullptr) {
                    x += Scale(kIconSize, dpi) + Scale(kIconTextGap, dpi);
                }
                const int availableWidth = (removeRect.left - Scale(kPaddingX, dpi)) - x;
                if (IsTitleTruncated(window_, row.title, availableWidth, dpi)) {
                    hoveredTitle = row.title;
                    text = hoveredTitle.c_str();
                }
            }
        }
    }

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = window_;
    ti.uId = 1;

    if (text == nullptr) {
        SendMessageW(tooltipWindow_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
        return;
    }
    // Applied fresh on every show, not just once at creation, so a theme
    // change without restarting Polish still takes effect -- same
    // reasoning as GroupChromeWindow::UpdateTooltip's own comment.
    ApplyDarkModeToTooltip(tooltipWindow_);
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tooltipWindow_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    POINT cursor{};
    GetCursorPos(&cursor);
    SendMessageW(tooltipWindow_, TTM_TRACKPOSITION, 0, MAKELPARAM(cursor.x + 12, cursor.y + 20));
    SendMessageW(tooltipWindow_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

}  // namespace polish
