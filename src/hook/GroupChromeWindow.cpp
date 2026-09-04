#include "hook/GroupChromeWindow.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

#include "resource.h"
#include "util/DarkMode.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupChromeWindow";
constexpr int kTabStripHeight = 36;   // logical (96 DPI) px
constexpr int kTabMinWidth = 120;     // logical px
constexpr int kTabMaxWidth = 220;     // logical px
constexpr int kTabStripLeftPadding = 8;  // logical px, before the first tab
// Must be >= kTabCornerRadius (below): the active tab's concave-fillet
// join to the connector band (DrawConcaveFillet) reaches
// kTabCornerRadius px to each side of it. A gap narrower than that --
// confirmed real, a 4px gap against an 8px radius -- let the fillet
// bleed into the *neighboring* tab's own bottom corner.
constexpr int kTabGap = 8;               // logical px, between adjacent tabs
constexpr int kTabCornerRadius = 8;      // logical px -- tabs' rounded top corners

// Tab mode only: a permanent full-width band between the tab strip and
// the member's own content, colored to match the active tab -- File
// Explorer's own command-bar area does the same thing. Real reserved
// space (subtracted from the content rect, not just painted over it),
// so it stays visible regardless of what the member itself renders.
constexpr int kTabConnectorHeight = 10;  // logical px

// Tile mode: the draggable resize splitters between tiles. Real space
// is reserved for these in GroupManager's own grid math (members never
// overlap them) -- kSplitterWidth is that reserved width, and also the
// rendered bar's width once hovered/dragged. kSplitterRestWidth is the
// thinner hairline drawn the rest of the time (Windows Terminal/VS
// Code/Settings-app convention: subtle at rest, grows and accents on
// hover) -- safe to be much thinner than kSplitterWidth purely visually
// since the reserved gap and hit-test target don't shrink with it, only
// the paint does. kSplitterHitSlop adds extra invisible margin on each
// side purely for hit-testing (confirmed real: an exactly-splitter-
// width click target was fiddly to grab even after widening the bar
// itself). kMinTileSize is the floor neither side of a drag can shrink
// past -- enough to still make out that a window is there, not just a
// sliver.
constexpr int kSplitterWidth = 8;      // logical px
constexpr int kSplitterRestWidth = 2;  // logical px
constexpr int kSplitterHitSlop = 4;    // logical px, each side
constexpr int kMinTileSize = 80;       // logical px

constexpr UINT_PTR kHoverTimerId = 1;
constexpr UINT kHoverDelayMs = 400;

// Local to this window's own context menu -- TrackPopupMenu's returned
// command isn't routed through WM_COMMAND, so these don't need to be
// unique app-wide.
constexpr UINT kContextMenuSwitchMode = 1;
constexpr UINT kContextMenuEditWindows = 2;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// Draws the concave quarter-circle join where a narrower element (the
// active tab) meets a wider surface below it (the full-width connector
// band) -- the exact transition Windows 11 File Explorer uses between
// its active tab and its command bar, rather than a sharp 90-degree
// corner. `cornerX/cornerY` is the outer point where the tab's side
// meets the band's top edge; `leftSide` selects the tab's bottom-left
// vs. bottom-right corner (the two are mirror images of each other).
//
// Method: near cornerX/cornerY, everything is `innerColor` (continuing
// both the tab's side and the band's top edge as one shape); a quarter
// circle of radius `radius`, centered on the *outer* far corner of that
// square, is then carved out in `outerColor` -- leaving a concave arc
// that curves from the tab's straight side into the band's straight top
// edge instead of meeting at a hard corner.
void DrawConcaveFillet(HDC hdc, int cornerX, int cornerY, int radius, COLORREF innerColor, COLORREF outerColor,
                       bool leftSide) {
    const RECT square = leftSide ? RECT{cornerX - radius, cornerY - radius, cornerX, cornerY}
                                  : RECT{cornerX, cornerY - radius, cornerX + radius, cornerY};
    HBRUSH innerBrush = CreateSolidBrush(innerColor);
    FillRect(hdc, &square, innerBrush);
    DeleteObject(innerBrush);

    const int farX = leftSide ? square.left : square.right;
    const int farY = square.top;
    HRGN circleRgn = CreateEllipticRgn(farX - radius, farY - radius, farX + radius, farY + radius);
    HRGN squareRgn = CreateRectRgn(square.left, square.top, square.right, square.bottom);
    CombineRgn(squareRgn, squareRgn, circleRgn, RGN_AND);
    HBRUSH outerBrush = CreateSolidBrush(outerColor);
    FillRgn(hdc, squareRgn, outerBrush);
    DeleteObject(outerBrush);
    DeleteObject(circleRgn);
    DeleteObject(squareRgn);
}

}  // namespace

GroupChromeWindow::GroupChromeWindow(HINSTANCE instance) : instance_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        // CS_DBLCLKS -- WM_LBUTTONDBLCLK never fires without it (default
        // is off); needed for double-click-to-toggle on a tile splitter.
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        // Same icon as the tray (TrayIcon::AddIcon) -- otherwise every
        // group chrome falls back to the generic default window icon in
        // its own title bar, taskbar button, and Alt+Tab entry, instead
        // of reading as a Polish window.
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_POLISH_TRAY));
        windowClass.hIconSm = windowClass.hIcon;
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
        case WM_SETCURSOR: {
            // Only for hovering a splitter -- everything else (the
            // window's own resize border, etc.) still needs its normal
            // default handling, so only intercept the plain client-area
            // case and only when a splitter is actually under the
            // cursor.
            if (mode_ == GroupMode::Tile && LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (const auto hit = HitTestSplitter(pt)) {
                    SetCursor(LoadCursorW(nullptr, hit->first ? IDC_SIZEWE : IDC_SIZENS));
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_ERASEBKGND:
            // The window class's default background brush (COLOR_WINDOW,
            // i.e. white) would otherwise paint on every erase -- visibly
            // for a frame -- before WM_PAINT's own PaintTabStrip repaints
            // over it with the real (often dark) colors. Confirmed real:
            // switching tabs showed a white flash on an otherwise dark
            // group. PaintTabStrip always fully repaints the client area
            // on its own, every time, so the default erase step is pure
            // overhead here -- returning nonzero tells Windows this
            // message was handled without actually erasing anything.
            return 1;

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
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (mode_ == GroupMode::Tile) {
                if (const auto hit = HitTestSplitter(pt)) {
                    draggingSplitter_ = *hit;
                    SetCapture(hwnd);
                }
                return 0;
            }
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
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
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (draggingSplitter_.has_value()) {
                DragSplitter(pt);
                return 0;
            }
            if (mode_ == GroupMode::Tile) {
                // Splitter hover highlight -- Tile mode has no tabs, so
                // this stands in for (doesn't compete with) the tab
                // hover-preview tracking below, which only ever applies
                // in Tab mode anyway (ComputeTabRects is always empty
                // here). Cursor shape itself is WM_SETCURSOR's job, not
                // this handler's.
                if (!trackingMouseLeave_) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    if (TrackMouseEvent(&tme)) {
                        trackingMouseLeave_ = true;
                    }
                }
                const auto newHover = HitTestSplitter(pt);
                if (newHover != hoveredSplitter_) {
                    if (hoveredSplitter_.has_value()) {
                        InvalidateSplitterBand(hoveredSplitter_->first, hoveredSplitter_->second);
                    }
                    hoveredSplitter_ = newHover;
                    if (hoveredSplitter_.has_value()) {
                        InvalidateSplitterBand(hoveredSplitter_->first, hoveredSplitter_->second);
                    }
                }
                return 0;
            }

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const std::vector<RECT> tabRects = ComputeTabRects(clientRect);

            if (draggingIndex_.has_value()) {
                for (size_t i = 0; i < tabRects.size(); ++i) {
                    RECT r = tabRects[i];
                    if (PtInRect(&r, pt) && i != *draggingIndex_) {
                        if (onTabReordered_) {
                            onTabReordered_(*draggingIndex_, i);
                        }
                        // The dragged tab is now at index i -- reorder
                        // fires live, once per crossing, not just once
                        // on drop (see SetOnTabReordered's comment).
                        draggingIndex_ = i;
                        break;
                    }
                }
                return 0;
            }

            // Hover-preview tracking (only when not mid-drag). Needs
            // WM_MOUSELEAVE to reliably notice the cursor leaving the
            // whole window (not just leaving a tab rect, which
            // WM_MOUSEMOVE's own coordinates can't distinguish from
            // "still over the window but not any tab") --
            // TrackMouseEvent must be re-armed on every WM_MOUSEMOVE
            // per its own documented usage pattern.
            if (!trackingMouseLeave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme)) {
                    trackingMouseLeave_ = true;
                }
            }

            std::optional<size_t> newHover;
            for (size_t i = 0; i < tabRects.size(); ++i) {
                RECT r = tabRects[i];
                if (PtInRect(&r, pt)) {
                    newHover = i;
                    break;
                }
            }
            if (newHover != hoveredTabIndex_) {
                hoveredTabIndex_ = newHover;
                // Tab strip only, not the whole window -- InvalidateRect
                // with a null rect also repaints the content-area band
                // behind the active member, visibly overwriting it until
                // something else forces it to repaint itself again (a
                // real, confirmed bug: moving the mouse off a tab in any
                // direction blanked File Explorer's content).
                InvalidateTabStrip();
                KillTimer(hwnd, kHoverTimerId);
                if (newHover.has_value()) {
                    SetTimer(hwnd, kHoverTimerId, kHoverDelayMs, nullptr);
                } else if (onTabHovered_) {
                    onTabHovered_(std::nullopt, RECT{});
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouseLeave_ = false;
            if (hoveredTabIndex_.has_value()) {
                hoveredTabIndex_.reset();
                InvalidateTabStrip();
                KillTimer(hwnd, kHoverTimerId);
                if (onTabHovered_) {
                    onTabHovered_(std::nullopt, RECT{});
                }
            }
            if (hoveredSplitter_.has_value()) {
                InvalidateSplitterBand(hoveredSplitter_->first, hoveredSplitter_->second);
                hoveredSplitter_.reset();
            }
            return 0;

        case WM_TIMER:
            if (wParam == kHoverTimerId) {
                KillTimer(hwnd, kHoverTimerId);
                if (hoveredTabIndex_.has_value() && onTabHovered_) {
                    RECT clientRect;
                    GetClientRect(hwnd, &clientRect);
                    const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
                    if (*hoveredTabIndex_ < tabRects.size()) {
                        RECT screenRect = tabRects[*hoveredTabIndex_];
                        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);
                        onTabHovered_(hoveredTabIndex_, screenRect);
                    }
                }
            }
            return 0;

        case WM_LBUTTONDBLCLK: {
            if (mode_ == GroupMode::Tile) {
                const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                auto hit = HitTestSplitter(pt);
                if (hit.has_value() && onTileSplitterDoubleClicked_) {
                    onTileSplitterDoubleClicked_(hit->first, hit->second);
                }
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_LBUTTONUP: {
            if (draggingSplitter_.has_value()) {
                ReleaseCapture();
                draggingSplitter_.reset();
                // One final full self-redraw once the drag actually
                // ends -- cheap here (once per drag, not once per
                // mouse-move like the reflows during the drag itself),
                // and guarantees a fully clean frame on both sides of
                // the splitter regardless of any transient repaint
                // race during the drag (confirmed real: visible update
                // artifacts lingering after release).
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
                return 0;
            }
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
            // than leaving draggingIndex_/draggingSplitter_ stuck set,
            // which would make the next unrelated WM_MOUSEMOVE misbehave.
            draggingIndex_.reset();
            draggingSplitter_.reset();
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

        case WM_SIZE:
            // Members are real children now -- they already move for
            // free when the chrome itself moves (no callback needed for
            // that at all, unlike the old reposition-only design). A
            // *resize* still needs an explicit relayout, since children
            // don't auto-resize to fill a bigger/smaller parent.
            if (onResized_) {
                onResized_();
            }
            return 0;

        case WM_DPICHANGED: {
            // Standard MSDN-documented handling: resize to the rect
            // Windows suggests for the new DPI (a Per-Monitor-V2-aware
            // window doesn't get resized automatically just because it
            // moved to a different-DPI monitor -- the app has to do it).
            // If the size actually changes, this SetWindowPos triggers
            // WM_SIZE on its own, which re-lays-out members above --
            // every layout/paint calculation already calls
            // GetDpiForWindow fresh rather than caching a stale DPI, so
            // no separate DPI-specific relayout path is needed here.
            const auto* suggestedRect = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggestedRect->left, suggestedRect->top,
                         suggestedRect->right - suggestedRect->left, suggestedRect->bottom - suggestedRect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_SETTINGCHANGE:
            // Fires when the user flips Settings > Personalization >
            // Colors > "Choose your mode" while a group is already open
            // -- re-apply the native title bar and repaint the
            // self-painted tab strip so both follow live, not just on
            // next creation. lParam names the changed setting as a
            // string ("ImmersiveColorSet" for a theme change), but
            // re-checking the registry directly is cheap enough to just
            // always do it rather than string-compare lParam.
            ApplyDarkTitleBar(hwnd, IsDarkModeEnabled());
            InvalidateRect(hwnd, nullptr, TRUE);
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_CLOSE:
            // Runs before DefWindowProcW's default WM_CLOSE handling
            // (which calls DestroyWindow) -- see SetOnClosing's comment
            // for why the owner must release members here, synchronously,
            // not after.
            if (onClosing_) {
                onClosing_();
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_DESTROY:
            // Not the app's main message window -- no PostQuitMessage
            // here.
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
    // Matches File Explorer/Notepad's own tab strip: a small inset
    // before the first tab, and a small gap between tabs (rather than
    // them sitting flush against each other and the window edge).
    const int leftPadding = Scale(kTabStripLeftPadding, dpi);
    const int tabGap = Scale(kTabGap, dpi);

    const int memberCount = static_cast<int>(memberTitles_.size());
    const int availableWidth = clientRect.right - clientRect.left - leftPadding - (memberCount - 1) * tabGap;
    int tabWidth = availableWidth / memberCount;
    tabWidth = std::max(tabMinWidth, std::min(tabWidth, tabMaxWidth));

    int x = clientRect.left + leftPadding;
    for (size_t i = 0; i < memberTitles_.size(); ++i) {
        if (x >= clientRect.right) {
            break;
        }
        rects.push_back(
            RECT{x, clientRect.top, std::min(x + tabWidth, static_cast<int>(clientRect.right)), clientRect.top + tabHeight});
        x += tabWidth + tabGap;
    }
    return rects;
}

void GroupChromeWindow::PaintTabStrip(HDC hdc, const RECT& clientRect) {
    // Windows 11's own tab style (File Explorer, Notepad): a strip the
    // tabs sit in, an active tab that's the *same* color as the content
    // area below it (so it visually merges/"grows out of" the content,
    // no border between them), rounded top corners only. Inactive tabs
    // now get their own (much darker) fill too, rather than just
    // showing the strip color -- background contrast is the primary way
    // the active tab stands out; text color only needs a slight nudge on
    // top of that, not a stark black-vs-white split.
    const bool dark = IsDarkModeEnabled();
    const COLORREF kContentColor = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xFF, 0xFF, 0xFF);
    const COLORREF kActiveTabColor = kContentColor;
    const COLORREF kInactiveTabColor = dark ? RGB(0x0A, 0x0A, 0x0A) : RGB(0xDD, 0xDD, 0xDD);
    const COLORREF kHoverTabColor = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xE9, 0xE9, 0xE9);
    const COLORREF kActiveBorderColor = dark ? RGB(0x3F, 0x3F, 0x3F) : RGB(0xD8, 0xD8, 0xD8);
    const COLORREF kActiveTextColor = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A);
    const COLORREF kInactiveTextColor = dark ? RGB(0xE0, 0xE0, 0xE0) : RGB(0x00, 0x00, 0x00);

    const UINT dpi = GetDpiForWindow(window_);
    const int cornerRadius = Scale(kTabCornerRadius, dpi);

    if (mode_ == GroupMode::Tile) {
        // No custom header at all in Tile mode -- there are no tabs to
        // render, and the native OS title bar already shows the
        // group's name (Name(), set via SetWindowTextW), so the plain
        // "Group (N window(s), tiled)" label this used to draw was
        // pure duplication. Removing it hands that space back to the
        // tiled members instead (see HeaderHeight, which returns 0 for
        // this mode).
        HBRUSH contentBrush = CreateSolidBrush(kContentColor);
        FillRect(hdc, &clientRect, contentBrush);
        DeleteObject(contentBrush);

        if (!tileColumnBoundaries_.empty() || !tileRowBoundaries_.empty()) {
            // Resting state matches the thin-hairline convention used by
            // Windows Terminal/VS Code/the Settings app -- the full
            // kSplitterWidth is still reserved as real gap space (layout
            // math and hit-testing are unchanged) and content already
            // fills the whole client rect including that gap, so a
            // resting splitter can draw as a much thinner line without
            // leaving a hole. Hovered (or actively being dragged) grows
            // to the full reserved width and switches to the OS's own
            // accent color -- the same visual cue Windows itself uses for
            // "this is draggable".
            const COLORREF accentColor = GetAccentColor();
            const int splitterWidth = Scale(kSplitterWidth, dpi);
            const int restWidth = Scale(kSplitterRestWidth, dpi);
            for (size_t i = 0; i < tileColumnBoundaries_.size(); ++i) {
                const bool highlighted = (draggingSplitter_.has_value() && draggingSplitter_->first &&
                                           draggingSplitter_->second == i) ||
                                          (hoveredSplitter_.has_value() && hoveredSplitter_->first &&
                                           hoveredSplitter_->second == i);
                const int width = highlighted ? splitterWidth : restWidth;
                const int boundary = tileColumnBoundaries_[i];
                RECT bar{clientRect.left + boundary - width / 2, clientRect.top,
                         clientRect.left + boundary + (width - width / 2), clientRect.bottom};
                HBRUSH splitterBrush = CreateSolidBrush(highlighted ? accentColor : kActiveBorderColor);
                FillRect(hdc, &bar, splitterBrush);
                DeleteObject(splitterBrush);
            }
            for (size_t i = 0; i < tileRowBoundaries_.size(); ++i) {
                const bool highlighted = (draggingSplitter_.has_value() && !draggingSplitter_->first &&
                                           draggingSplitter_->second == i) ||
                                          (hoveredSplitter_.has_value() && !hoveredSplitter_->first &&
                                           hoveredSplitter_->second == i);
                const int width = highlighted ? splitterWidth : restWidth;
                const int boundary = tileRowBoundaries_[i];
                RECT bar{clientRect.left, clientRect.top + boundary - width / 2, clientRect.right,
                         clientRect.top + boundary + (width - width / 2)};
                HBRUSH splitterBrush = CreateSolidBrush(highlighted ? accentColor : kActiveBorderColor);
                FillRect(hdc, &bar, splitterBrush);
                DeleteObject(splitterBrush);
            }
        }
        return;
    }

    const int tabHeight = Scale(kTabStripHeight, dpi);

    // The strip's own base fill is the inactive-tab color, not a
    // separate "strip background" color -- so the left padding before
    // the first tab, the gaps between tabs, and any leftover space past
    // the last tab all read as "more inactive tab" instead of a
    // visually distinct empty band. Active/hover tabs simply draw their
    // own fill on top.
    RECT stripRect{clientRect.left, clientRect.top, clientRect.right, clientRect.top + tabHeight};
    HBRUSH stripBrush = CreateSolidBrush(kInactiveTabColor);
    FillRect(hdc, &stripRect, stripBrush);
    DeleteObject(stripBrush);

    // Tab mode only: a permanent full-width band, colored to match the
    // active tab, between the strip and the member's own content --
    // File Explorer's own command-bar area does the same thing (see
    // kTabConnectorHeight's comment). Straight edges all the way to the
    // window's own left/right edges -- rounding those corners (tried
    // first) left an unpainted notch outside the round arc but inside
    // the clipped rect, showing whatever stale content was underneath
    // (a real, confirmed white artifact at the window's edges). Only
    // the tab-to-band join itself (DrawConcaveFillet, below) needs a
    // curve.
    if (mode_ == GroupMode::Tab) {
        RECT bandRect{clientRect.left, stripRect.bottom, clientRect.right, stripRect.bottom + Scale(kTabConnectorHeight, dpi)};
        HBRUSH bandBrush = CreateSolidBrush(kActiveTabColor);
        FillRect(hdc, &bandRect, bandBrush);
        DeleteObject(bandBrush);
    }

    RECT contentRect{clientRect.left, clientRect.top + HeaderHeight(dpi), clientRect.right, clientRect.bottom};
    HBRUSH contentBrush = CreateSolidBrush(kContentColor);
    FillRect(hdc, &contentRect, contentBrush);
    DeleteObject(contentBrush);

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
    if (tabRects.empty()) {
        SelectObject(hdc, oldFont);
        return;
    }

    const int iconSize = Scale(16, dpi);
    const int iconTextGap = Scale(4, dpi);

    // The active tab's label is bold, matching File Explorer/Notepad --
    // inactive tabs keep the regular weight already selected into hdc.
    LOGFONTW boldLogFont{};
    GetObjectW(font, sizeof(boldLogFont), &boldLogFont);
    boldLogFont.lfWeight = FW_BOLD;
    HFONT boldFont = CreateFontIndirectW(&boldLogFont);

    for (size_t i = 0; i < tabRects.size(); ++i) {
        const RECT& tabRect = tabRects[i];
        const bool active = (i == activeIndex_);
        const bool hovered = !active && hoveredTabIndex_.has_value() && *hoveredTabIndex_ == i;

        {
            const COLORREF fill = active ? kActiveTabColor : (hovered ? kHoverTabColor : kInactiveTabColor);
            HBRUSH tabBrush = CreateSolidBrush(fill);
            HGDIOBJ oldBrush = SelectObject(hdc, tabBrush);
            HPEN tabPen = active ? CreatePen(PS_SOLID, 1, kActiveBorderColor) : CreatePen(PS_NULL, 0, 0);
            HGDIOBJ oldPen = SelectObject(hdc, tabPen);
            // Rounded top corners only: RoundRect rounds all four
            // corners of whatever rect it's given, so the bottom
            // corners are pushed below tabRect.bottom (outside the
            // visible tab) before drawing, then clipped back to
            // tabRect's real bounds -- draws past the clip and gets cut
            // off cleanly, rather than needing a custom top-only-
            // rounded path. Without the clip, a non-active tab's fill
            // color -- unlike the active tab's, which matches the
            // content area exactly -- would visibly bleed a sliver into
            // the content area below.
            IntersectClipRect(hdc, tabRect.left, tabRect.top, tabRect.right, tabRect.bottom);
            RoundRect(hdc, tabRect.left, tabRect.top, tabRect.right, tabRect.bottom + cornerRadius, cornerRadius,
                      cornerRadius);
            SelectClipRgn(hdc, nullptr);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(tabBrush);
            if (!active) {
                DeleteObject(tabPen);
            }
        }

        if (active) {
            // Bridges the active tab into the connector band below it
            // (painted earlier, above) with the same concave join File
            // Explorer uses, instead of the sharp corner a plain
            // rectangle-meets-rectangle join would leave. Also covers
            // the active tab's own border-pen line at that seam (drawn
            // above, clipped exactly to tabRect.bottom).
            DrawConcaveFillet(hdc, tabRect.left, tabRect.bottom, cornerRadius, kActiveTabColor, kInactiveTabColor,
                               /*leftSide=*/true);
            DrawConcaveFillet(hdc, tabRect.right, tabRect.bottom, cornerRadius, kActiveTabColor, kInactiveTabColor,
                               /*leftSide=*/false);
        }

        RECT textRect = tabRect;
        InflateRect(&textRect, -Scale(8, dpi), 0);

        const HICON icon = (i < memberIcons_.size()) ? memberIcons_[i] : nullptr;
        if (icon != nullptr) {
            const int iconY = tabRect.top + ((tabRect.bottom - tabRect.top) - iconSize) / 2;
            DrawIconEx(hdc, textRect.left, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            textRect.left += iconSize + iconTextGap;
        }

        SetTextColor(hdc, active ? kActiveTextColor : kInactiveTextColor);
        SelectObject(hdc, active ? boldFont : font);
        DrawTextW(hdc, memberTitles_[i].c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    SelectObject(hdc, oldFont);
    DeleteObject(boldFont);
}

int GroupChromeWindow::HeaderHeight(UINT dpi) const {
    if (mode_ != GroupMode::Tab) {
        // Tile mode has no custom header at all -- see PaintTabStrip's
        // own comment (the native title bar already shows the group's
        // name, and there are no tabs to render).
        return 0;
    }
    return Scale(kTabStripHeight, dpi) + Scale(kTabConnectorHeight, dpi);
}

RECT GroupChromeWindow::ContentRectInScreenCoords() const {
    if (window_ == nullptr) {
        return RECT{};
    }
    RECT client;
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);

    POINT topLeft{client.left, client.top + HeaderHeight(dpi)};
    POINT bottomRight{client.right, client.bottom};
    ClientToScreen(window_, &topLeft);
    ClientToScreen(window_, &bottomRight);
    return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

RECT GroupChromeWindow::ContentRectInClientCoords() const {
    if (window_ == nullptr) {
        return RECT{};
    }
    RECT client;
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    return RECT{client.left, client.top + HeaderHeight(dpi), client.right, client.bottom};
}

void GroupChromeWindow::InvalidateTabStrip() {
    if (window_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    RECT headerRect{client.left, client.top, client.right, client.top + HeaderHeight(dpi)};
    InvalidateRect(window_, &headerRect, TRUE);
}

void GroupChromeWindow::SetTileSplitters(std::vector<int> columnBoundaries, std::vector<int> rowBoundaries) {
    if (window_ != nullptr) {
        // A band around every OLD and NEW boundary position -- not the
        // whole window, which also repaints the content-area fill
        // behind the members and visibly overwrites them (the same
        // class of bug already fixed for the tab strip's own hover
        // highlight; see InvalidateTabStrip's comment).
        const RECT contentRect = ContentRectInClientCoords();
        const UINT dpi = GetDpiForWindow(window_);
        const int band = Scale(kSplitterWidth + kSplitterHitSlop, dpi);
        for (const std::vector<int>* boundaries : {&tileColumnBoundaries_, &columnBoundaries}) {
            for (int x : *boundaries) {
                RECT bar{contentRect.left + x - band, contentRect.top, contentRect.left + x + band,
                         contentRect.bottom};
                InvalidateRect(window_, &bar, FALSE);
            }
        }
        for (const std::vector<int>* boundaries : {&tileRowBoundaries_, &rowBoundaries}) {
            for (int y : *boundaries) {
                RECT bar{contentRect.left, contentRect.top + y - band, contentRect.right,
                         contentRect.top + y + band};
                InvalidateRect(window_, &bar, FALSE);
            }
        }
    }
    tileColumnBoundaries_ = std::move(columnBoundaries);
    tileRowBoundaries_ = std::move(rowBoundaries);
}

int GroupChromeWindow::TileSplitterWidthPx() const {
    if (window_ == nullptr) {
        return 0;
    }
    return Scale(kSplitterWidth, GetDpiForWindow(window_));
}

std::optional<std::pair<bool, size_t>> GroupChromeWindow::HitTestSplitter(POINT clientPt) const {
    if (mode_ != GroupMode::Tile || window_ == nullptr) {
        return std::nullopt;
    }
    const RECT contentRect = ContentRectInClientCoords();
    const UINT dpi = GetDpiForWindow(window_);
    const int slop = Scale(kSplitterHitSlop, dpi);

    for (size_t i = 0; i < tileColumnBoundaries_.size(); ++i) {
        const int x = contentRect.left + tileColumnBoundaries_[i];
        if (clientPt.x >= x - slop && clientPt.x <= x + slop && clientPt.y >= contentRect.top &&
            clientPt.y <= contentRect.bottom) {
            return std::make_pair(true, i);
        }
    }
    for (size_t i = 0; i < tileRowBoundaries_.size(); ++i) {
        const int y = contentRect.top + tileRowBoundaries_[i];
        if (clientPt.y >= y - slop && clientPt.y <= y + slop && clientPt.x >= contentRect.left &&
            clientPt.x <= contentRect.right) {
            return std::make_pair(false, i);
        }
    }
    return std::nullopt;
}

void GroupChromeWindow::DragSplitter(POINT clientPt) {
    if (!draggingSplitter_.has_value() || window_ == nullptr) {
        return;
    }
    const auto [isColumn, index] = *draggingSplitter_;
    const std::vector<int>& boundaries = isColumn ? tileColumnBoundaries_ : tileRowBoundaries_;
    if (index >= boundaries.size()) {
        return;
    }
    const RECT contentRect = ContentRectInClientCoords();
    const UINT dpi = GetDpiForWindow(window_);
    const int minSize = Scale(kMinTileSize, dpi);
    const int splitterWidth = Scale(kSplitterWidth, dpi);
    const int totalSize = isColumn ? (contentRect.right - contentRect.left) : (contentRect.bottom - contentRect.top);

    // Only this boundary's own two neighbors bound how far it can
    // move -- everything past them belongs to a different pair and
    // stays fixed (matches GroupManager::SetTileBoundary's own
    // "only the adjacent pair changes" contract).
    const int prevBoundary = (index == 0) ? 0 : boundaries[index - 1];
    const int nextBoundary = (index + 1 < boundaries.size()) ? boundaries[index + 1] : totalSize;

    // Each side must leave room for both the *neighboring* splitter's
    // own reserved gap and this tile's minimum content size -- minSize
    // alone would let a tile shrink below the floor by up to one
    // splitter's width.
    const int lo = prevBoundary + splitterWidth + minSize;
    const int hi = nextBoundary - splitterWidth - minSize;
    if (lo >= hi) {
        return;  // no room left to move this splitter without violating the floor
    }
    const int rawPosition = isColumn ? (clientPt.x - contentRect.left) : (clientPt.y - contentRect.top);
    const int clamped = std::clamp(rawPosition, lo, hi);

    if (onTileSplitterDragged_) {
        onTileSplitterDragged_(isColumn, index, clamped);
    }
}

void GroupChromeWindow::InvalidateSplitterBand(bool column, size_t index) {
    if (window_ == nullptr) {
        return;
    }
    const std::vector<int>& boundaries = column ? tileColumnBoundaries_ : tileRowBoundaries_;
    if (index >= boundaries.size()) {
        return;
    }
    const RECT contentRect = ContentRectInClientCoords();
    const UINT dpi = GetDpiForWindow(window_);
    const int band = Scale(kSplitterWidth + kSplitterHitSlop, dpi);
    if (column) {
        const int x = contentRect.left + boundaries[index];
        RECT bar{x - band, contentRect.top, x + band, contentRect.bottom};
        InvalidateRect(window_, &bar, FALSE);
    } else {
        const int y = contentRect.top + boundaries[index];
        RECT bar{contentRect.left, y - band, contentRect.right, y + band};
        InvalidateRect(window_, &bar, FALSE);
    }
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
    // A no-op when nothing actually changed -- confirmed real via
    // diagnostic logging that a reparented member (a modern Notepad
    // instance) fires EVENT_OBJECT_NAMECHANGE for its own title
    // continuously (root OS cause unconfirmed, but real and repeatable:
    // idObject/idChild are already filtered to the window's own title,
    // not some noisier child control), even though the title string
    // itself never changes. Each such event drove OnMemberTitleChanged
    // -> here -> an unconditional full-window InvalidateRect, which is
    // exactly the nonstop visible flashing a user reported and
    // confirmed live. Comparing first turns a continuous stream of
    // no-op events into a single real update whenever the title
    // actually differs.
    if (titles == memberTitles_) {
        return;
    }
    memberTitles_ = titles;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void GroupChromeWindow::SetMemberIcons(const std::vector<HICON>& icons) {
    // Same no-op-when-unchanged guard as SetMemberTitles, and for the
    // same confirmed reason -- GetWindowIconHandle returns a handle
    // owned by the window/class (stable across calls for the same
    // icon, not a fresh copy each time), so comparing HICON values
    // directly is a valid change check, not just an approximation.
    if (icons == memberIcons_) {
        return;
    }
    memberIcons_ = icons;
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
    AppendMenuW(menu, MF_STRING, kContextMenuEditWindows, L"Edit Group Windows...");

    ApplyDarkModeToMenu(window_);

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
        // WS_CLIPCHILDREN -- confirmed real via diagnostic logging: a
        // CaptureThumbnail call's own RedrawWindow(..., RDW_ERASE) on a
        // *hidden* member (the tab being hovered, not the active one)
        // was bubbling up into this window's own client area, forcing a
        // full-client WM_PAINT that happened to cover the active
        // member's rect too -- the parent never intentionally paints
        // there (both Tab and Tile mode only ever draw in the header/
        // gap space around members, never over them), so without this
        // style there was nothing stopping that from visually
        // interfering with the active member's own on-screen content.
        // This is the standard Win32 fix for a parent with child
        // windows it never means to paint over.
        window_ = CreateWindowExW(0, kWindowClassName, L"Group", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1200, 850, nullptr, nullptr, instance_, this);
        if (window_ != nullptr) {
            ApplyDarkTitleBar(window_, IsDarkModeEnabled());
        }
    }
    if (window_ == nullptr) {
        return;
    }

    InvalidateRect(window_, nullptr, TRUE);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
}

}  // namespace polish
