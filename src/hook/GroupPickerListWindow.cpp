#include "hook/GroupPickerListWindow.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>

#include "util/DarkMode.h"
#include "util/UiFont.h"
#include "util/WindowIcon.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupPickerListWindow";

// Logical (96 DPI) px -- scaled fresh at every layout/paint via Scale(),
// never cached, same convention every other custom-painted window in
// this codebase uses.
constexpr int kRowHeight = 40;
constexpr int kPaddingX = 14;
constexpr int kIconSize = 20;
constexpr int kIconTextGap = 10;
constexpr int kRowCornerRadius = 6;
// Button hit/draw box -- reserved at the row's right edge always (see
// AltTabListWindow's own kActionButtonSize comment for why: keeps the
// title column's right edge aligned across every row regardless of
// whether a given row's button happens to be enabled).
constexpr int kButtonSize = 24;
constexpr int kButtonCornerRadius = 6;
constexpr int kGlyphMargin = 7;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// Linear per-channel blend toward `tint` -- used to give a selected row
// an accent-tinted fill distinct from a merely-hovered row's plain
// hoverColor (confirmed, human-reported: the two looked identical before
// this, with no way to tell "selected" from "the mouse happens to be
// here"). Same shape as GroupPickerWindow.cpp's own DarkenColor, just
// blending toward a second color instead of black.
COLORREF BlendColor(COLORREF base, COLORREF tint, double amount) {
    const int r = static_cast<int>(GetRValue(base) * (1 - amount) + GetRValue(tint) * amount);
    const int g = static_cast<int>(GetGValue(base) * (1 - amount) + GetGValue(tint) * amount);
    const int b = static_cast<int>(GetBValue(base) * (1 - amount) + GetBValue(tint) * amount);
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
}

// Full title, for the tooltip UpdateTooltip shows when a row's own text
// is too long for its column (DT_END_ELLIPSIS truncates it) -- measured
// with the exact font Paint draws it with, not GetStockObject's default,
// so this agrees with what's actually visible.
bool IsTitleTruncated(HWND window, const std::wstring& title, int availableWidth, UINT dpi) {
    HFONT font = MakeUiFont(dpi);
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

RECT ComputeAddButtonRect(const RECT& rowRect, UINT dpi) {
    const int buttonSize = Scale(kButtonSize, dpi);
    const int paddingX = Scale(kPaddingX, dpi);
    const int top = rowRect.top + (rowRect.bottom - rowRect.top - buttonSize) / 2;
    const int left = rowRect.right - paddingX - buttonSize;
    return RECT{left, top, left + buttonSize, top + buttonSize};
}

}  // namespace

GroupPickerListWindow::GroupPickerListWindow(HINSTANCE instance) : instance_(instance) {
    static bool commonControlsInitialized = false;
    if (!commonControlsInitialized) {
        // ICC_TAB_CLASSES, not the more obviously-named ICC_*TOOLTIP*
        // flag -- comctl32 groups the tooltip common control in with tab
        // controls historically; see GroupChromeWindow's own constructor
        // for the same call.
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
        commonControlsInitialized = true;
    }
}

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
        // CS_DBLCLKS -- WM_LBUTTONDBLCLK never fires without it (off by
        // default); needed for double-click-to-add on a row.
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
    window_ = CreateWindowExW(0, kWindowClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, parent,
                               nullptr, instance_, this);

    // One manually-tracked tool for the single Add button that's ever
    // visible at a time -- see GroupChromeWindow::UpdateTooltip/Create
    // for the identical TTF_TRACK-based technique this mirrors.
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

        case WM_SETFOCUS:
            hasFocus_ = true;
            // Tabbing into a list with nothing selected in it should still
            // leave a row to arrow from -- and makes that row's Add button
            // visible, the same "always show a selected row so its action
            // button stays discoverable" reasoning RefreshLists' own
            // fallback already applies at the dialog level.
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
            // Right is this list's half of the move gesture -- the
            // keyboard equivalent of clicking the row's own Add button.
            // Space does the same thing: it's the conventional "activate
            // the focused row" key, and a list where the only way to act
            // on a row is an arrow key reads as broken to anyone who
            // tries the obvious one first. Left does nothing here: a
            // window in Open windows can only move right, into Group
            // (see the sibling list's own VK_LEFT case for the other
            // half).
            if (wParam == VK_RIGHT || wParam == VK_SPACE) {
                if (selectedIndex_.has_value() && onAddRequested_) {
                    onAddRequested_(rows_[*selectedIndex_].hwnd);
                }
                return 0;
            }
            if (wParam != VK_UP && wParam != VK_DOWN && wParam != VK_HOME && wParam != VK_END &&
                wParam != VK_PRIOR && wParam != VK_NEXT) {
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            if (rows_.empty()) {
                return 0;
            }
            // Clamped at both ends rather than wrapping: Tab is what
            // leaves this list (see GroupPickerWindow::CycleFocus), so
            // wrapping here would just make the two gestures ambiguous.
            // With nothing selected yet, every one of these keys lands on
            // the first row -- the same "first keypress has a
            // well-defined destination" rule CycleFocus follows.
            size_t next = 0;
            if (selectedIndex_.has_value()) {
                const size_t current = *selectedIndex_;
                const size_t last = rows_.size() - 1;
                const size_t page = RowsPerPage();
                switch (wParam) {
                    case VK_UP:
                        next = current == 0 ? 0 : current - 1;
                        break;
                    case VK_DOWN:
                        next = std::min(current + 1, last);
                        break;
                    case VK_HOME:
                        next = 0;
                        break;
                    case VK_END:
                        next = last;
                        break;
                    case VK_PRIOR:
                        next = current < page ? 0 : current - page;
                        break;
                    case VK_NEXT:
                        next = std::min(current + page, last);
                        break;
                    default:
                        next = current;
                        break;
                }
            }
            // Reported up rather than applied here, exactly as a row-body
            // click is -- GroupPickerWindow owns the single cross-list
            // selection and pushes it back down to both panels.
            if (onRowSelected_) {
                onRowSelected_(rows_[next].hwnd);
            }
            EnsureRowVisible(next);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // Keep keyboard focus with whichever panel the mouse last
            // acted on, so arrowing after a click continues from the row
            // just clicked instead of from some other panel's selection.
            SetFocus(hwnd);
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pt.y += scrollOffset_;
            const UINT dpi = GetDpiForWindow(hwnd);
            const RowLayout layout = ComputeLayout(dpi);
            // Only the selected-or-hovered row(s) ever draw an Add
            // button (see Paint) -- mirror that same set here, same
            // shape as AltTabListWindow's own hit-test loop over
            // {highlightIndex_, hoveredIndex_}.
            for (std::optional<size_t> rowIndex : {selectedIndex_, hoveredIndex_}) {
                if (!rowIndex.has_value() || *rowIndex >= layout.rowRects.size() || *rowIndex >= rows_.size()) {
                    continue;
                }
                const RECT& r = layout.rowRects[*rowIndex];
                RECT addRect = ComputeAddButtonRect(r, dpi);
                if (PtInRect(&addRect, pt)) {
                    if (onAddRequested_) {
                        onAddRequested_(rows_[*rowIndex].hwnd);
                    }
                    return 0;
                }
            }
            // Not on a visible Add button -- select whichever row (if
            // any) was actually clicked. GroupPickerWindow owns what
            // "selected" means across both lists; this just reports the
            // gesture (see SetOnRowSelected's own comment).
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

        case WM_LBUTTONDBLCLK: {
            // A quick way to add a window without having to land the
            // first click precisely on the row's own small Add button --
            // double-clicking anywhere on the row body does the same
            // thing (requires CS_DBLCLKS on this class, see the
            // constructor).
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pt.y += scrollOffset_;
            const RowLayout layout = ComputeLayout(GetDpiForWindow(hwnd));
            for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
                if (PtInRect(&layout.rowRects[i], pt)) {
                    if (onAddRequested_) {
                        onAddRequested_(rows_[i].hwnd);
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
    const COLORREF buttonBorderColor = dark ? RGB(0x70, 0x70, 0x70) : RGB(0x90, 0x90, 0x90);
    const COLORREF accentColor = GetAccentColor();
    // Distinct from hoverColor -- see BlendColor's own comment.
    const COLORREF selectedColor = BlendColor(backgroundColor, accentColor, 0.35);

    HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
    FillRect(hdc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    // Everything below (until the viewport origin is restored) is drawn
    // in ComputeLayout's "natural" unshifted coordinates -- see
    // AltTabListWindow::Paint's own comment on this same technique.
    POINT priorViewportOrg{};
    SetViewportOrgEx(hdc, 0, -scrollOffset_, &priorViewportOrg);

    HFONT textFont = MakeUiFont(dpi);
    HGDIOBJ oldFont = SelectObject(hdc, textFont);
    SetBkMode(hdc, TRANSPARENT);

    const RowLayout layout = ComputeLayout(dpi);
    const int paddingX = Scale(kPaddingX, dpi);
    const int iconSize = Scale(kIconSize, dpi);
    const int iconTextGap = Scale(kIconTextGap, dpi);
    const int rowCornerRadius = Scale(kRowCornerRadius, dpi);
    const int buttonCornerRadius = Scale(kButtonCornerRadius, dpi);
    const int glyphMargin = Scale(kGlyphMargin, dpi);

    for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
        const RECT& rowRect = layout.rowRects[i];
        const GroupPickerRow& row = rows_[i];
        const bool hovered = hoveredIndex_.has_value() && *hoveredIndex_ == i;
        const bool selected = selectedIndex_.has_value() && *selectedIndex_ == i;
        const bool showButton = selected || hovered;
        const int rowMidY = (rowRect.top + rowRect.bottom) / 2;

        // Always a full-bleed rounded fill covering the *entire* row
        // width -- selectedColor takes priority over hoverColor when a
        // row is somehow both (confirmed, human-reported: before this,
        // selected and merely-hovered rows were visually identical, with
        // no way to tell which one was actually selected) -- drawn
        // before anything else, so every later element (the Add button's
        // outline/glyph included) paints on top of a guaranteed-correct,
        // fully opaque surface with no gap a button's own small
        // bounding box could leave unpainted.
        {
            const COLORREF fillColor = selected ? selectedColor : (hovered ? hoverColor : backgroundColor);
            HBRUSH fillBrush = CreateSolidBrush(fillColor);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldBrush = SelectObject(hdc, fillBrush);
            HGDIOBJ oldPen = SelectObject(hdc, nullPen);
            RoundRect(hdc, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, rowCornerRadius, rowCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(fillBrush);
        }

        int x = rowRect.left + paddingX;
        if (row.icon != nullptr) {
            const int iconTop = rowMidY - iconSize / 2;
            DrawIconEx(hdc, x, iconTop, row.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            x += iconSize + iconTextGap;
        }

        const RECT addRect = ComputeAddButtonRect(rowRect, dpi);
        RECT textRect{x, rowRect.top, addRect.left - paddingX, rowRect.bottom};
        SetTextColor(hdc, textColor);
        DrawTextW(hdc, row.title.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        // Add button: a right-pointing arrow -- move this window toward
        // the Selected panel, which sits above this one. Only drawn (and
        // only hit-testable, see WM_LBUTTONDOWN) on the selected-or-
        // hovered row -- the reserved textRect width above stays
        // constant regardless, so text never reflows as the button
        // appears/disappears on different rows.
        if (showButton) {
            HPEN buttonPen = CreatePen(PS_SOLID, 1, buttonBorderColor);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            HGDIOBJ oldPen = SelectObject(hdc, buttonPen);
            RoundRect(hdc, addRect.left, addRect.top, addRect.right, addRect.bottom, buttonCornerRadius,
                      buttonCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(buttonPen);

            HPEN glyphPen = CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), accentColor);
            HGDIOBJ oldGlyphPen = SelectObject(hdc, glyphPen);
            const int gLeft = addRect.left + glyphMargin;
            const int gRight = addRect.right - glyphMargin;
            const int gMidY = (addRect.top + addRect.bottom) / 2;
            MoveToEx(hdc, gLeft, gMidY, nullptr);
            LineTo(hdc, gRight, gMidY);
            MoveToEx(hdc, gRight - Scale(5, dpi), gMidY - Scale(5, dpi), nullptr);
            LineTo(hdc, gRight, gMidY);
            LineTo(hdc, gRight - Scale(5, dpi), gMidY + Scale(5, dpi));
            SelectObject(hdc, oldGlyphPen);
            DeleteObject(glyphPen);
        }
    }

    SetViewportOrgEx(hdc, priorViewportOrg.x, priorViewportOrg.y, nullptr);
    SelectObject(hdc, oldFont);
    DeleteObject(textFont);

    // An accent-colored, thicker border while this panel owns keyboard
    // focus -- the only focus cue available when the list is empty (no
    // selected row to highlight), and the counterpart to the focus rect
    // Windows already draws on the dialog's own buttons.
    HPEN borderPen = hasFocus_ ? CreatePen(PS_SOLID, std::max(1, Scale(2, dpi)), GetAccentColor())
                                : CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldBorderPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBorderBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, clientRect.left, clientRect.top, clientRect.right, clientRect.bottom);
    SelectObject(hdc, oldBorderPen);
    SelectObject(hdc, oldBorderBrush);
    DeleteObject(borderPen);
}

// Scrolls the minimum distance needed to bring row `index` fully into
// view -- no-op when it's already visible. Reuses the same clamped
// scrollOffset_/SetScrollInfo/invalidate shape WM_MOUSEWHEEL and
// WM_VSCROLL already use; only the target offset is computed differently.
size_t GroupPickerListWindow::RowsPerPage() const {
    // How far PageUp/PageDown moves: one viewport's worth of rows, the
    // conventional meaning, so paging lines up with what's actually on
    // screen rather than an arbitrary fixed count. At least 1, so a
    // viewport shorter than a single row still advances instead of
    // leaving the key dead.
    if (window_ == nullptr) {
        return 1;
    }
    RECT clientRect;
    GetClientRect(window_, &clientRect);
    const int rowHeight = Scale(kRowHeight, GetDpiForWindow(window_));
    if (rowHeight <= 0) {
        return 1;
    }
    return std::max<size_t>(1, static_cast<size_t>((clientRect.bottom - clientRect.top) / rowHeight));
}

void GroupPickerListWindow::EnsureRowVisible(size_t index) {
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

void GroupPickerListWindow::SetWindows(const std::vector<HWND>& candidates) {
    rows_.clear();
    rows_.reserve(candidates.size());
    for (HWND hwnd : candidates) {
        rows_.push_back(GroupPickerRow{hwnd, GetWindowTitle(hwnd), GetWindowIconHandle(hwnd)});
    }
    hoveredIndex_.reset();
    // selectedIndex_ deliberately left untouched here -- GroupPickerWindow
    // always follows a SetWindows call with SetSelectedHwnd, recomputing
    // it fresh against the new rows_ (see that method and this class's
    // own header comment on why selection isn't decided in here).
    scrollOffset_ = 0;
    if (window_ != nullptr) {
        UpdateScrollInfo();
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupPickerListWindow::SetSelectedHwnd(std::optional<HWND> hwnd) {
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

void GroupPickerListWindow::UpdateTooltip() {
    if (tooltipWindow_ == nullptr) {
        return;
    }

    // Two tiers: the Add button's own tooltip, when hovered; otherwise,
    // if the hovered row's title doesn't fit its column (DT_END_ELLIPSIS
    // truncated it), show the full title -- helpful precisely when it's
    // chopped off, so this deliberately doesn't fire for a title that
    // already fits (see IsTitleTruncated's own comment).
    const wchar_t* text = nullptr;
    std::wstring hoveredTitle;
    if (hoveredIndex_.has_value() && window_ != nullptr) {
        const UINT dpi = GetDpiForWindow(window_);
        const RowLayout layout = ComputeLayout(dpi);
        if (*hoveredIndex_ < layout.rowRects.size() && *hoveredIndex_ < rows_.size()) {
            const GroupPickerRow& row = rows_[*hoveredIndex_];
            const RECT& rowRect = layout.rowRects[*hoveredIndex_];
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            cursor.y += scrollOffset_;
            const RECT addRect = ComputeAddButtonRect(rowRect, dpi);
            if (PtInRect(&addRect, cursor)) {
                text = L"Add to group";
            } else {
                int x = Scale(kPaddingX, dpi);
                if (row.icon != nullptr) {
                    x += Scale(kIconSize, dpi) + Scale(kIconTextGap, dpi);
                }
                const int availableWidth = (addRect.left - Scale(kPaddingX, dpi)) - x;
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
