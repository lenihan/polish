#include "hook/AltTabListWindow.h"

#include <shellscalingapi.h>
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
// Extra vertical gap inserted once, between the last active row and the
// first minimized row -- only present when both sections are non-empty
// (see ComputeLayout, the one place this and kHeaderHeight are turned
// into actual rects).
constexpr int kSectionGapHeight = 10;
// Height of the "Active"/"Minimized" heading above each non-empty
// section -- smaller than kRowHeight since it carries a single small
// label, not an icon+title row.
constexpr int kHeaderHeight = 22;
// Section heading font is the same face as row text (see Paint), just
// shrunk relative to it -- a second distinct system font would be
// overkill for one label.
constexpr double kHeaderFontScale = 0.85;

// Keyboard-shortcut legend below the last row -- always present, in the
// same small muted style as the section headings (see kHeaderFontScale/
// kHeaderTextColor, both reused rather than a fourth font/color pair for
// one more small label).
constexpr int kFooterHeight = 22;
constexpr int kFooterTopGap = 6;
constexpr wchar_t kFooterLegendText[] = L"Del: Close    -: Minimize    +: Maximize";

// Per-row action-button hit target (square) -- reserved at the right
// edge of *every* row (see class comment on why: keeps row text width
// constant regardless of which row is currently highlighted/hovered, or
// how many buttons a given row actually shows), but only ever drawn for
// the highlighted row or whichever row the mouse is currently over.
// Reserved width always accounts for all three buttons (see
// ActionsReservedWidth) even on a minimized row, which only ever draws
// two -- keeping every row's text right edge aligned matters more here
// than reclaiming a few px of text width on minimized rows specifically.
// Glyphs are drawn a few px smaller than the button box itself so they
// don't touch its edges.
constexpr int kActionButtonSize = 20;
constexpr int kActionButtonGap = 6;
constexpr int kActionButtonGlyphMargin = 5;
constexpr int kActionButtonCount = 3;

// The whole window (background AND row content) is rendered at this one
// constant alpha -- a "frosted, see-through-but-legible" panel doesn't
// need per-pixel alpha the way AltTabHighlightBorder's gradient ring
// does, so this reuses AltTabDimOverlay's simpler flat-alpha technique
// instead of the DIB+GDI+ pipeline.
constexpr BYTE kPanelAlpha = 235;

constexpr COLORREF kBackgroundColor = RGB(32, 32, 32);
constexpr COLORREF kTextColor = RGB(240, 240, 240);
constexpr COLORREF kHighlightTextColor = RGB(255, 255, 255);
// Muted relative to kTextColor -- reads as "not currently on screen"
// without needing a second font or per-pixel alpha (the panel's own
// alpha is a single flat value across the whole window, see kPanelAlpha).
constexpr COLORREF kMinimizedTextColor = RGB(165, 165, 165);
constexpr COLORREF kSectionDividerColor = RGB(80, 80, 80);
constexpr COLORREF kHeaderTextColor = RGB(150, 150, 150);
// A subtler fill than the highlighted row's solid accent color -- just
// enough to signal "the mouse is over this row" without it reading as
// "this is what Tab would land on next" (that's the accent color's job).
constexpr COLORREF kHoverBackgroundColor = RGB(55, 55, 55);

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// Horizontal space reserved for the three action buttons at every row's
// right edge (see kActionButtonSize's own comment) -- one shared formula
// so the button-rect helpers below and Paint's row text width can never
// drift apart on how much space is actually set aside.
int ActionsReservedWidth(UINT dpi) {
    return kActionButtonCount * Scale(kActionButtonSize, dpi) + (kActionButtonCount - 1) * Scale(kActionButtonGap, dpi);
}

// Pure functions of a row's own rect (plus dpi) -- deliberately not
// dependent on which row is highlighted/hovered, so both Paint and
// WM_LBUTTONDOWN's hit-test can compute the exact same rects for
// whichever row(s) need them (currently: the highlighted row and/or the
// hovered row, which may be the same row, different rows, or either one
// alone) without a caller having to first ask "is this the interesting
// row" the way a single cached RowLayout field once did.
RECT ComputeCloseButtonRect(const RECT& rowRect, UINT dpi) {
    const int buttonSize = Scale(kActionButtonSize, dpi);
    const int rowPaddingX = Scale(kRowPaddingX, dpi);
    const int buttonTop = rowRect.top + (Scale(kRowHeight, dpi) - buttonSize) / 2;
    const int closeLeft = rowRect.right - rowPaddingX - buttonSize;
    return RECT{closeLeft, buttonTop, closeLeft + buttonSize, buttonTop + buttonSize};
}

RECT ComputeMinimizeToggleButtonRect(const RECT& rowRect, UINT dpi) {
    const RECT close = ComputeCloseButtonRect(rowRect, dpi);
    const int buttonSize = Scale(kActionButtonSize, dpi);
    const int gap = Scale(kActionButtonGap, dpi);
    const int toggleLeft = close.left - gap - buttonSize;
    return RECT{toggleLeft, close.top, toggleLeft + buttonSize, close.bottom};
}

// Leftmost of the three -- only ever drawn/hit-tested on an
// active-section row (see class comment: maximize/restore doesn't apply
// to a minimized row).
RECT ComputeMaximizeToggleButtonRect(const RECT& rowRect, UINT dpi) {
    const RECT minimizeToggle = ComputeMinimizeToggleButtonRect(rowRect, dpi);
    const int buttonSize = Scale(kActionButtonSize, dpi);
    const int gap = Scale(kActionButtonGap, dpi);
    const int left = minimizeToggle.left - gap - buttonSize;
    return RECT{left, minimizeToggle.top, left + buttonSize, minimizeToggle.bottom};
}

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
            const RowLayout layout = ComputeLayout(dpi);

            // Action-button hit targets take priority over the row-body
            // hit-test below -- they only ever exist on the highlighted
            // row and/or the hovered row (see class comment), and
            // neither should also be treated as a row-body click (which
            // would activate/commit instead of toggling minimize or
            // closing). Checked for both rows even when they're the same
            // one -- harmless duplicate work, not worth a branch to skip.
            for (std::optional<size_t> rowIndex : {highlightIndex_, hoveredIndex_}) {
                if (!rowIndex.has_value() || *rowIndex >= rows_.size() || *rowIndex >= layout.rowRects.size()) {
                    continue;
                }
                const RECT& rowRect = layout.rowRects[*rowIndex];
                RECT closeRect = ComputeCloseButtonRect(rowRect, dpi);
                if (PtInRect(&closeRect, pt)) {
                    if (onRowClose_) {
                        onRowClose_(rows_[*rowIndex].hwnd);
                    }
                    return 0;
                }
                RECT toggleRect = ComputeMinimizeToggleButtonRect(rowRect, dpi);
                if (PtInRect(&toggleRect, pt)) {
                    if (onRowMinimizeToggle_) {
                        onRowMinimizeToggle_(rows_[*rowIndex].hwnd);
                    }
                    return 0;
                }
                // Maximize/restore-toggle only exists on an active-section
                // row -- see class comment.
                if (!rows_[*rowIndex].minimized) {
                    RECT maximizeRect = ComputeMaximizeToggleButtonRect(rowRect, dpi);
                    if (PtInRect(&maximizeRect, pt)) {
                        if (onRowMaximizeToggle_) {
                            onRowMaximizeToggle_(rows_[*rowIndex].hwnd);
                        }
                        return 0;
                    }
                }
            }

            for (size_t i = 0; i < layout.rowRects.size() && i < rows_.size(); ++i) {
                RECT r = layout.rowRects[i];  // PtInRect takes a non-const RECT*
                if (PtInRect(&r, pt)) {
                    if (onRowActivated_) {
                        onRowActivated_(rows_[i].hwnd);
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            // TrackMouseEvent is (re-)armed on every move rather than
            // once -- it's a one-shot subscription per MSDN (cleared the
            // moment it fires), so it must be re-requested after every
            // WM_MOUSELEAVE, and re-arming it redundantly on moves in
            // between is harmless.
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);

            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
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

        case WM_MOUSELEAVE:
            SetHoveredIndex(std::nullopt);
            return 0;

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

AltTabListWindow::RowLayout AltTabListWindow::ComputeLayout(UINT dpi) const {
    RowLayout layout;
    layout.rowRects.reserve(rows_.size());
    const int paddingX = Scale(kPanelPaddingX, dpi);
    const int paddingY = Scale(kPanelPaddingY, dpi);
    const int rowHeight = Scale(kRowHeight, dpi);
    const int headerHeight = Scale(kHeaderHeight, dpi);
    const int sectionGap = Scale(kSectionGapHeight, dpi);
    const int width = Scale(kPanelWidth, dpi) - 2 * paddingX;

    const bool hasActiveRow = std::any_of(rows_.begin(), rows_.end(), [](const AltTabListRow& r) { return !r.minimized; });

    int top = paddingY;
    if (hasActiveRow) {
        layout.activeHeaderRect = RECT{paddingX, top, paddingX + width, top + headerHeight};
        top += headerHeight;
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        // First minimized row reached -- insert its section heading here.
        // A gap only precedes it when active rows came before (i.e. this
        // isn't the very first thing in the panel); a monitor panel with
        // only minimized candidates needs no such gap.
        if (rows_[i].minimized && !layout.minimizedHeaderRect.has_value()) {
            if (i > 0) {
                top += sectionGap;
            }
            layout.minimizedHeaderRect = RECT{paddingX, top, paddingX + width, top + headerHeight};
            top += headerHeight;
        }
        layout.rowRects.push_back(RECT{paddingX, top, paddingX + width, top + rowHeight});
        top += rowHeight;
    }

    if (!rows_.empty()) {
        top += Scale(kFooterTopGap, dpi);
        const int footerHeight = Scale(kFooterHeight, dpi);
        layout.footerRect = RECT{paddingX, top, paddingX + width, top + footerHeight};
        top += footerHeight;
    }
    layout.contentHeight = top + paddingY;
    return layout;
}

void AltTabListWindow::Paint(HDC hdc, const RECT& clientRect) const {
    const UINT dpi = GetDpiForWindow(window_);

    HBRUSH backgroundBrush = CreateSolidBrush(kBackgroundColor);
    FillRect(hdc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    // A plain memory DC (see WM_PAINT) has no font selected of its own,
    // so it falls back to whatever stock font GDI defaults to -- a tiny,
    // pre-DPI-awareness bitmap font, nowhere near the size real Windows
    // UI text renders at. NONCLIENTMETRICS' lfMessageFont is the actual
    // font Windows itself uses for dialog body text, read via the
    // DPI-aware overload so it's already the correct size for this
    // window's current monitor -- not GetStockObject(DEFAULT_GUI_FONT),
    // which is the same era of fixed, non-DPI-scaled stock font as the
    // implicit default and would look just as undersized on a modern
    // display.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT textFont = CreateFontIndirectW(&metrics.lfMessageFont);

    // Section headings reuse the row font's face, just shrunk -- see
    // kHeaderFontScale. lfHeight stays negative (its usual "match this
    // character height" convention) after scaling since it's just scaled
    // in magnitude.
    LOGFONTW headerLogFont = metrics.lfMessageFont;
    headerLogFont.lfHeight = static_cast<LONG>(headerLogFont.lfHeight * kHeaderFontScale);
    headerLogFont.lfWeight = FW_SEMIBOLD;
    HFONT headerFont = CreateFontIndirectW(&headerLogFont);

    HGDIOBJ oldFont = SelectObject(hdc, textFont);
    SetBkMode(hdc, TRANSPARENT);
    const RowLayout layout = ComputeLayout(dpi);
    const int iconSize = Scale(kIconSize, dpi);
    const int rowPaddingX = Scale(kRowPaddingX, dpi);
    const int iconTextGap = Scale(kIconTextGap, dpi);
    const int rowCornerRadius = Scale(kRowCornerRadius, dpi);
    const int sectionGap = Scale(kSectionGapHeight, dpi);
    const COLORREF accentColor = GetAccentColor();

    SelectObject(hdc, headerFont);
    SetTextColor(hdc, kHeaderTextColor);
    if (layout.activeHeaderRect) {
        RECT r = *layout.activeHeaderRect;
        r.left += rowPaddingX;
        DrawTextW(hdc, L"Active", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    if (layout.minimizedHeaderRect) {
        // A divider line above the heading, but only when it's not the
        // very first thing in the panel (a monitor with only minimized
        // candidates has nothing above it to divide from).
        if (layout.activeHeaderRect) {
            const int dividerY = layout.minimizedHeaderRect->top - sectionGap / 2;
            HPEN dividerPen = CreatePen(PS_SOLID, 1, kSectionDividerColor);
            HGDIOBJ oldPen = SelectObject(hdc, dividerPen);
            MoveToEx(hdc, layout.minimizedHeaderRect->left, dividerY, nullptr);
            LineTo(hdc, layout.minimizedHeaderRect->right, dividerY);
            SelectObject(hdc, oldPen);
            DeleteObject(dividerPen);
        }
        RECT r = *layout.minimizedHeaderRect;
        r.left += rowPaddingX;
        DrawTextW(hdc, L"Minimized", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(hdc, textFont);

    // Glyphs are drawn with plain 1px lines in kHighlightTextColor --
    // buttons only ever appear on a row with some background fill behind
    // them (the highlighted row's solid accent color, or a hovered row's
    // subtler kHoverBackgroundColor), so this one bright color is
    // guaranteed to contrast with either.
    auto drawGlyphLine = [hdc](int x1, int y1, int x2, int y2) {
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
    };

    const int actionsReservedWidth = ActionsReservedWidth(dpi);
    const std::vector<RECT>& rowRects = layout.rowRects;
    for (size_t i = 0; i < rowRects.size() && i < rows_.size(); ++i) {
        const RECT& rowRect = rowRects[i];
        const bool highlighted = (highlightIndex_.has_value() && *highlightIndex_ == i);
        const bool hovered = (hoveredIndex_.has_value() && *hoveredIndex_ == i);

        if (highlighted || hovered) {
            HBRUSH fillBrush = CreateSolidBrush(highlighted ? accentColor : kHoverBackgroundColor);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldBrush = SelectObject(hdc, fillBrush);
            HGDIOBJ oldPen = SelectObject(hdc, nullPen);
            RoundRect(hdc, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, rowCornerRadius,
                      rowCornerRadius);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(fillBrush);
        }

        int textLeft = rowRect.left + rowPaddingX;
        const AltTabListRow& row = rows_[i];
        if (row.icon != nullptr) {
            const int iconTop = rowRect.top + (rowRect.bottom - rowRect.top - iconSize) / 2;
            DrawIconEx(hdc, textLeft, iconTop, row.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            textLeft += iconSize + iconTextGap;
        }

        RECT textRect{textLeft, rowRect.top, rowRect.right - rowPaddingX - actionsReservedWidth, rowRect.bottom};
        SetTextColor(hdc, highlighted ? kHighlightTextColor : (row.minimized ? kMinimizedTextColor : kTextColor));
        DrawTextW(hdc, row.title.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (highlighted || hovered) {
            const RECT toggle = ComputeMinimizeToggleButtonRect(rowRect, dpi);
            const RECT close = ComputeCloseButtonRect(rowRect, dpi);
            const COLORREF rowFillColor = highlighted ? accentColor : kHoverBackgroundColor;
            HPEN glyphPen = CreatePen(PS_SOLID, 1, kHighlightTextColor);
            HGDIOBJ oldPen = SelectObject(hdc, glyphPen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            const int margin = Scale(kActionButtonGlyphMargin, dpi);
            // Minimize/restore-toggle glyph: always the same single
            // horizontal line near the bottom of the box (the native
            // title-bar minimize glyph), on an active row and a minimized
            // one alike -- rather than a second, unfamiliar glyph for
            // "restore from minimized", the button just keeps reading as
            // "the minimize control" in both of its states.
            {
                const int y = toggle.bottom - margin;
                drawGlyphLine(toggle.left + margin, y, toggle.right - margin, y);
            }

            if (!row.minimized) {
                // Maximize/restore-toggle, active rows only -- native
                // Windows glyph shapes: a single square outline to
                // maximize, two overlapping offset squares to restore.
                const RECT maximizeToggle = ComputeMaximizeToggleButtonRect(rowRect, dpi);
                if (IsZoomed(row.hwnd)) {
                    // The native restore glyph's back square is only ever
                    // partly visible -- its bottom-left portion sits
                    // behind the front square. Filling the front square
                    // with the row's own background color before
                    // outlining it erases whatever of the back square's
                    // outline would otherwise show through underneath,
                    // the same opaque-front-face look the native glyph
                    // has (plain NULL_BRUSH outlines would just show both
                    // squares' lines crossing through each other instead).
                    const int offset = Scale(3, dpi);
                    Rectangle(hdc, maximizeToggle.left + margin + offset, maximizeToggle.top + margin,
                              maximizeToggle.right - margin, maximizeToggle.bottom - margin - offset);
                    HBRUSH occludeBrush = CreateSolidBrush(rowFillColor);
                    SelectObject(hdc, occludeBrush);
                    Rectangle(hdc, maximizeToggle.left + margin, maximizeToggle.top + margin + offset,
                              maximizeToggle.right - margin - offset, maximizeToggle.bottom - margin);
                    SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    DeleteObject(occludeBrush);
                } else {
                    Rectangle(hdc, maximizeToggle.left + margin, maximizeToggle.top + margin,
                              maximizeToggle.right - margin, maximizeToggle.bottom - margin);
                }
            }

            drawGlyphLine(close.left + margin, close.top + margin, close.right - margin, close.bottom - margin);
            drawGlyphLine(close.right - margin, close.top + margin, close.left + margin, close.bottom - margin);

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(glyphPen);
        }
    }

    if (layout.footerRect) {
        SelectObject(hdc, headerFont);
        SetTextColor(hdc, kHeaderTextColor);
        RECT r = *layout.footerRect;
        DrawTextW(hdc, kFooterLegendText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(textFont);
    DeleteObject(headerFont);
}

void AltTabListWindow::Reposition(HMONITOR targetMonitor, UINT dpi) {
    const int width = Scale(kPanelWidth, dpi);
    const int height = ComputeLayout(dpi).contentHeight;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT workArea{0, 0, width, height};
    if (GetMonitorInfoW(targetMonitor, &monitorInfo)) {
        workArea = monitorInfo.rcWork;
    }
    const int x = workArea.left + (workArea.right - workArea.left - width) / 2;
    const int y = workArea.top + (workArea.bottom - workArea.top - height) / 2;

    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void AltTabListWindow::Show(const std::vector<AltTabListRow>& rows, std::optional<size_t> highlightIndex,
                             HMONITOR targetMonitor) {
    if (window_ == nullptr) {
        return;
    }
    rows_ = rows;
    highlightIndex_ = (highlightIndex.has_value() && *highlightIndex < rows_.size()) ? highlightIndex : std::nullopt;

    // GetDpiForMonitor rather than GetDpiForWindow(window_) -- this panel
    // is about to move to targetMonitor, which may not be the monitor
    // window_ currently sits on (its previous session could have shown
    // it somewhere else, or it could still be at its startup pre-warm
    // position), so window_'s own current DPI isn't necessarily correct
    // for the monitor it's about to be sized for.
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    GetDpiForMonitor(targetMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    Reposition(targetMonitor, dpiX);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void AltTabListWindow::SetHighlight(std::optional<size_t> index) {
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
    if (index.has_value() && *index >= rows_.size()) {
        index = std::nullopt;
    }
    if (index == highlightIndex_) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    const std::vector<RECT> rowRects = ComputeLayout(dpi).rowRects;
    // Narrow invalidate -- just the row(s) that actually change look, not
    // the whole panel. This codebase has hit the "unconditional full
    // repaint causes visible flashing" bug shape more than once already
    // (the tab strip's own hover highlight, a member-title-change
    // repaint); no reason to reintroduce it here.
    const std::optional<size_t> oldIndex = highlightIndex_;
    highlightIndex_ = index;
    if (oldIndex.has_value() && *oldIndex < rowRects.size()) {
        RECT r = rowRects[*oldIndex];
        InvalidateRect(window_, &r, FALSE);
    }
    if (index.has_value() && *index < rowRects.size()) {
        RECT r = rowRects[*index];
        InvalidateRect(window_, &r, FALSE);
    }
}

void AltTabListWindow::SetHoveredIndex(std::optional<size_t> index) {
    if (index.has_value() && *index >= rows_.size()) {
        index = std::nullopt;
    }
    if (index == hoveredIndex_) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    const std::vector<RECT> rowRects = ComputeLayout(dpi).rowRects;
    const std::optional<size_t> oldIndex = hoveredIndex_;
    hoveredIndex_ = index;
    // Same narrow-invalidate shape as SetHighlight -- a full-panel
    // repaint on every mouse-move over the list would be wasteful and
    // this codebase has already hit that exact flashing bug shape more
    // than once elsewhere.
    if (oldIndex.has_value() && *oldIndex < rowRects.size()) {
        RECT r = rowRects[*oldIndex];
        InvalidateRect(window_, &r, FALSE);
    }
    if (index.has_value() && *index < rowRects.size()) {
        RECT r = rowRects[*index];
        InvalidateRect(window_, &r, FALSE);
    }
}

void AltTabListWindow::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
    // Otherwise a stale hoveredIndex_ from before this Hide() could point
    // at the wrong row (or draw buttons prematurely) the moment the panel
    // is shown again somewhere else, before the OS gets around to sending
    // a fresh WM_MOUSEMOVE for wherever the cursor actually is now.
    hoveredIndex_ = std::nullopt;
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
