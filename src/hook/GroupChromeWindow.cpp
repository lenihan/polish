#include "hook/GroupChromeWindow.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupChromeWindow";
constexpr int kTabStripHeight = 36;   // logical (96 DPI) px
constexpr int kTabMinWidth = 120;     // logical px
constexpr int kTabMaxWidth = 220;     // logical px
constexpr int kTabStripLeftPadding = 8;  // logical px, before the first tab
constexpr int kTabGap = 4;               // logical px, between adjacent tabs

constexpr UINT_PTR kHoverTimerId = 1;
constexpr UINT kHoverDelayMs = 400;

// Local to this window's own context menu -- TrackPopupMenu's returned
// command isn't routed through WM_COMMAND, so these don't need to be
// unique app-wide.
constexpr UINT kContextMenuSwitchMode = 1;
constexpr UINT kContextMenuEditWindows = 2;

// Some SDK headers don't yet define this (added Windows 10 20H1) --
// the numeric value is stable/documented, safe to fall back to.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// The user's actual chosen app theme (Settings > Personalization >
// Colors > "Choose your mode"), not just assumed light -- confirmed
// necessary: a hardcoded light palette looked jarringly out of place
// sitting in an otherwise all-dark desktop. Same registry value every
// dark-mode-aware Win32 app reads; no public API for it.
bool IsDarkModeEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ,
                       &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value),
                                      &size) == ERROR_SUCCESS &&
                     type == REG_DWORD;
    RegCloseKey(key);
    return ok && value == 0;
}

// Applies (or removes) the dark native title bar/frame to match --
// otherwise the chrome's own OS-drawn title bar stays light even when
// everything this app paints itself, and every other app on screen, is
// dark.
void ApplyDarkTitleBar(HWND hwnd, bool dark) {
    BOOL enabled = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
}

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
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

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

    RECT contentRect{clientRect.left, clientRect.top + tabHeight, clientRect.right, clientRect.bottom};
    HBRUSH contentBrush = CreateSolidBrush(kContentColor);
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
        SetTextColor(hdc, kActiveTextColor);
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

    const int iconSize = Scale(16, dpi);
    const int iconTextGap = Scale(4, dpi);
    const int cornerRadius = Scale(8, dpi);

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

RECT GroupChromeWindow::ContentRectInClientCoords() const {
    if (window_ == nullptr) {
        return RECT{};
    }
    RECT client;
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int tabHeight = Scale(kTabStripHeight, dpi);
    return RECT{client.left, client.top + tabHeight, client.right, client.bottom};
}

void GroupChromeWindow::InvalidateTabStrip() {
    if (window_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int tabHeight = Scale(kTabStripHeight, dpi);
    RECT stripRect{client.left, client.top, client.right, client.top + tabHeight};
    InvalidateRect(window_, &stripRect, TRUE);
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

void GroupChromeWindow::SetMemberIcons(const std::vector<HICON>& icons) {
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
                                   CW_USEDEFAULT, 1200, 850, nullptr, nullptr, instance_, this);
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
