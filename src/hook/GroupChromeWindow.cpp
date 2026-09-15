#include "hook/GroupChromeWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <format>
#include <iterator>

#include "resource.h"
#include "util/DarkMode.h"
#include "util/Logging.h"
#include "util/UiFont.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupChromeWindow";
// The chrome's initial size on first Show() -- logical (96 DPI) px, NOT
// the raw pixels CreateWindowExW itself takes (see Show's own comment on
// why these get DPI-scaled explicitly right after creation, rather than
// passed to CreateWindowExW as-is).
constexpr int kDefaultChromeWidth = 1200;
constexpr int kDefaultChromeHeight = 850;
// Height of the self-painted title bar band that replaces the native OS
// caption -- see the class comment (GroupChromeWindow.h) for the
// shrink-the-caption-via-WM_NCCALCSIZE technique this relies on. A first
// pass at 32 (chosen to read as a normal Windows 11 title bar) was part
// of what got confirmed "huge" on a real 200% display, alongside
// kTitleBarButtonWidth's own oversized first guess -- trimmed down as
// part of the same live tuning pass.
constexpr int kTitleBarHeight = 28;  // logical (96 DPI) px
constexpr int kTabStripHeight = 36;   // logical (96 DPI) px
constexpr int kTabMinWidth = 120;     // logical px
constexpr int kTabMaxWidth = 220;     // logical px
constexpr int kTabStripLeftPadding = 8;  // logical px, before the first tab
// Must be >= kTabCornerRadius (below): the active tab's concave-fillet
// join to the connector band (DrawConcaveFillet) reaches
// kTabCornerRadius px to each side of it. A gap narrower than that --
// confirmed real, a 4px gap against an 8px radius -- let the fillet
// bleed into the *neighboring* tab's own bottom corner. Applies on both
// axes: Vertical's fillets reach the same distance above and below the
// active tab, into the same gap, so the one constant covers both.
constexpr int kTabGap = 8;               // logical px, between adjacent tabs
constexpr int kTabCornerRadius = 8;      // logical px -- tabs' rounded top corners

// The active tab's light outline, in *device* pixels -- deliberately not
// DPI-scaled, unlike almost everything else here. Three separate pieces
// have to line up pixel-for-pixel to read as one continuous stroke: the
// tab's own RoundRect pen, the concave fillets' arc
// (DrawConcaveFillet's borderThickness), and the straight run along the
// connector's outer edge out to the header's edges (PaintTabStrip). One
// constant so they can't drift apart. Scale this if the line reads too
// faint at high DPI -- all three follow.
constexpr int kActiveBorderThickness = 1;  // device px

// Tab mode only: a permanent full-length band between the tab strip and
// the member's own content, colored to match the active tab -- File
// Explorer's own command-bar area does the same thing. Real reserved
// space (subtracted from the content rect, not just painted over it),
// so it stays visible regardless of what the member itself renders.
// Height in Horizontal (a band under the top row), width in Vertical (a
// column right of the left-edge strip) -- same role on either axis, see
// PaintTabStrip's two branches.
constexpr int kTabConnectorThickness = 10;  // logical px

// Vertical alignment only: the tab strip becomes a left-edge column of
// this fixed width instead of a top row auto-sized to each label (a
// vertical tab list conventionally uses a fixed column, e.g. Firefox's
// own vertical tabs -- simpler than trying to auto-size a column's
// width to whichever label is longest).
constexpr int kTabStripWidth = 200;  // logical px

// Tile/Stack: the draggable resize splitters between tiles. Real space
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

// Title-bar band content -- icon, title text, and the three caption
// buttons (see PaintTitleBar/MinimizeButtonRect/MaximizeButtonRect/
// CloseButtonRect). An initial pass at kTitleBarButtonWidth=46 (a
// commonly-quoted native Windows 10/11 caption button width) plus an
// 8px glyph margin was still reported "huge" even after a first
// shrink pass (46->32, margin 8->6) -- turned out the button *box* was
// already fine (smaller than Windows' own SM_CXSIZE metric at the same
// DPI), but the *glyph drawn inside it* wasn't: measuring a live 2x-DPI
// screenshot pixel-for-pixel against VS Code's own custom title bar
// (same monitor, same DPI, a known-good reference) found our close-X
// glyph at ~40x31 physical px versus VS Code's ~19x19 -- because the
// old approach sized the glyph as "button box minus a fixed margin", so
// a bigger button box directly meant a bigger glyph. Real title bars
// don't scale the glyph with the hit-box: the glyph is a small, fixed
// size regardless of how big the clickable button is. kTitleBarGlyphSize
// is that fixed glyph bounding box, centered within each button rect
// (see GlyphRect below) instead of being derived from it.
constexpr int kTitleBarButtonWidth = 32;      // logical px
constexpr int kTitleBarIconSize = 16;         // logical px -- matches the small icon a native caption shows
constexpr int kTitleBarIconLeftPadding = 10;  // logical px, before the icon
constexpr int kTitleBarIconTextGap = 8;       // logical px, between icon and title text
// Fixed glyph bounding box (independent of button size -- see comment
// above) and the offset between the two overlapping squares of the
// restore-from-maximized glyph.
constexpr int kTitleBarGlyphSize = 10;
constexpr int kTitleBarRestoreGlyphOffset = 2;
// Windows' own "close" red -- used only for the close button's hover
// fill, in both light and dark mode (native Windows 11 does the same:
// the close button is the one caption button whose hover color doesn't
// follow the theme).
constexpr COLORREF kTitleBarCloseHoverColor = RGB(0xE8, 0x11, 0x23);
// Vertical inset (top and bottom) of the thin separator line drawn
// between the mode-toggle/manage-windows buttons and the min/max/close
// caption buttons -- shorter than the full band height, the same
// "doesn't touch the edges" convention as most apps' toolbar separators.
constexpr int kTitleBarSeparatorInset = 8;  // logical px

// Local to this window's own context menu -- TrackPopupMenu's returned
// command isn't routed through WM_COMMAND, so these don't need to be
// unique app-wide. The three mode ids are contiguous (kContextMenuModeTab
// first) so CheckMenuRadioItem can address them as one radio group by
// range.
constexpr UINT kContextMenuModeTab = 1;
constexpr UINT kContextMenuModeTile = 2;
constexpr UINT kContextMenuModeStack = 3;
constexpr UINT kContextMenuEditWindows = 4;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// Whether `hitCode` (a WM_SETCURSOR lParam LOWORD, or a WM_NCHITTEST
// result) is one of the eight resize-border/corner codes -- used to
// recognize when a *member's own frame* is asking for a resize cursor
// (see WM_SETCURSOR's own comment on why the chrome has to care).
bool IsResizeHitCode(WORD hitCode) {
    switch (hitCode) {
        case HTLEFT:
        case HTRIGHT:
        case HTTOP:
        case HTTOPLEFT:
        case HTTOPRIGHT:
        case HTBOTTOM:
        case HTBOTTOMLEFT:
        case HTBOTTOMRIGHT:
            return true;
        default:
            return false;
    }
}

// hwnd:"title"[class] -- the same shape WindowReparenting.cpp's own
// DescribeWindow uses, for the same reason: the class name is what makes
// a UWP host frame (ApplicationFrameWindow) recognizable in the log.
std::wstring DescribeWindow(HWND hwnd) {
    wchar_t title[128] = L"";
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    wchar_t className[128] = L"";
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return std::format(L"{}:\"{}\"[{}]", reinterpret_cast<void*>(hwnd), title, className);
}

// Draws the concave quarter-circle join where a narrower element (the
// active tab) meets a wider surface beside it (the full-length connector
// band/column) -- the exact transition Windows 11 File Explorer uses
// between its active tab and its command bar, rather than a sharp
// 90-degree corner. Axis-agnostic: `square` is the radius x radius block
// adjacent to the corner where the tab's edge meets the connector's edge
// (one of the tab's four corners, whichever one this call is bridging);
// `arcCenter` is that square's own corner diagonally opposite the
// meeting corner -- the quarter circle carved out in `outerColor` is
// centered there. Horizontal's two calls (tab bottom-left/bottom-right
// meeting the band below) and Vertical's two calls (tab top-right/
// bottom-right meeting the column to its right) are just different
// square/arcCenter pairs into the same shared logic.
//
// Method: `square` starts out entirely `innerColor` (continuing both the
// tab's straight edge and the connector's own straight edge as one
// shape); a quarter circle of radius `radius`, centered on `arcCenter`,
// is filled in `borderColor`, then a second, smaller quarter circle
// (radius - borderThickness) is filled in `outerColor` on top of that --
// leaving a `borderThickness`-wide ring of `borderColor` tracing the arc
// exactly, the curved counterpart of the active tab's own straight-edge
// pen (PaintTabStrip) and the straight run along the connector's outer
// edge (also PaintTabStrip) -- together, one continuous outline with no
// step where curve meets straight line. Pass borderThickness <= 0 (or
// >= radius) for the plain two-fill join with no visible border, the
// original behavior.
void DrawConcaveFillet(HDC hdc, const RECT& square, POINT arcCenter, COLORREF innerColor, COLORREF outerColor,
                       COLORREF borderColor, int borderThickness) {
    HBRUSH innerBrush = CreateSolidBrush(innerColor);
    FillRect(hdc, &square, innerBrush);
    DeleteObject(innerBrush);

    const int radius = square.right - square.left;
    HRGN squareRgn = CreateRectRgn(square.left, square.top, square.right, square.bottom);

    HRGN outerCircleRgn =
        CreateEllipticRgn(arcCenter.x - radius, arcCenter.y - radius, arcCenter.x + radius, arcCenter.y + radius);
    HRGN outerRgn = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(outerRgn, squareRgn, outerCircleRgn, RGN_AND);
    HBRUSH borderBrush = CreateSolidBrush(borderColor);
    FillRgn(hdc, outerRgn, borderBrush);
    DeleteObject(borderBrush);
    DeleteObject(outerCircleRgn);
    DeleteObject(outerRgn);

    const int innerRadius = radius - borderThickness;
    if (innerRadius > 0) {
        HRGN innerCircleRgn = CreateEllipticRgn(arcCenter.x - innerRadius, arcCenter.y - innerRadius,
                                                arcCenter.x + innerRadius, arcCenter.y + innerRadius);
        HRGN innerRgn = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(innerRgn, squareRgn, innerCircleRgn, RGN_AND);
        HBRUSH outerBrush = CreateSolidBrush(outerColor);
        FillRgn(hdc, innerRgn, outerBrush);
        DeleteObject(outerBrush);
        DeleteObject(innerCircleRgn);
        DeleteObject(innerRgn);
    }
    DeleteObject(squareRgn);
}

}  // namespace

GroupChromeWindow::GroupChromeWindow(HINSTANCE instance) : instance_(instance) {
    static bool commonControlsInitialized = false;
    if (!commonControlsInitialized) {
        // ICC_TAB_CLASSES, not the more obviously-named ICC_*TOOLTIP*
        // flag -- comctl32 groups the tooltip common control in with tab
        // controls historically; this is the documented way to make
        // TOOLTIPS_CLASSW available (see UpdateTooltip/Show).
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
        commonControlsInitialized = true;
    }

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
        case WM_NCCALCSIZE: {
            // wParam is FALSE (a single RECT*, no repositioning
            // capability) the first time this fires during a window's
            // own creation -- confirmed live: skipping the override in
            // that case entirely (as an earlier version of this code did)
            // left the native caption at full height and fully
            // functional, since the TRUE-only override never ran until
            // some later resize/move, which this window may never
            // actually get. Both cases are handled the same way below:
            // get DefWindowProcW's own answer first (correct left/right/
            // bottom resize-border insets either way -- WS_THICKFRAME
            // stays in place, so edge/corner resize keeps working exactly
            // as it does natively), then shrink just its *top* -- sized
            // for the full native caption, exactly the ~31px band this
            // class reclaims to paint its own title bar into (see the
            // class comment's shrink-not-remove technique) -- down to a
            // thin resize-border sliver, handing the rest of that space
            // back as ordinary client area.
            RECT* rect = (wParam == TRUE) ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
                                            : reinterpret_cast<RECT*>(lParam);
            const RECT requestedRect = *rect;
            const LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
            // Maximized windows are a documented, known gotcha here:
            // Windows deliberately sizes a maximized top-level window a
            // few px past the actual monitor work area on every side (so
            // the invisible resize border lands off-screen), and
            // DefWindowProcW's own top inset above already accounts for
            // that specifically for this window's current maximized
            // rect. Overriding it with our own thin-border constant
            // (which knows nothing about that offset) would leave real
            // content clipped at the monitor edge -- confirmed exactly
            // this failure mode in early testing of this exact technique
            // elsewhere, not something to rediscover here. Only override
            // when not maximized; DefWindowProcW's own maximized-case
            // top is already correct as-is.
            if (!IsZoomed(hwnd)) {
                const UINT dpi = GetDpiForWindow(hwnd);
                const int topBorder =
                    GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                rect->top = requestedRect.top + topBorder;
            }
            return (wParam == TRUE) ? result : 0;
        }

        case WM_NCHITTEST: {
            const LRESULT defaultHit = DefWindowProcW(hwnd, message, wParam, lParam);
            // Only a genuine resize-border/corner hit is passed through --
            // WS_THICKFRAME's own hit-testing for those is computed from
            // the outer window rect and the OS resize-frame thickness,
            // independent of caption height, so it still works unchanged
            // even though the caption itself was shrunk above. Anything
            // else DefWindowProcW might report here -- HTCAPTION,
            // HTMINBUTTON, HTMAXBUTTON, HTCLOSE, HTSYSMENU -- is computed
            // against the *original*, un-shrunk caption geometry (DPI/
            // theme metrics it tracks on its own, not the client rect
            // WM_NCCALCSIZE just changed), which is exactly what let a
            // click land on the native minimize button and actually
            // minimize the window even after this class's own
            // WM_NCCALCSIZE override -- confirmed live: the native
            // buttons kept working, unshrunk, until this exclusion was
            // added. Every one of those must be re-decided by this
            // class's own logic below instead.
            switch (defaultHit) {
                case HTLEFT:
                case HTRIGHT:
                case HTTOP:
                case HTTOPLEFT:
                case HTTOPRIGHT:
                case HTBOTTOM:
                case HTBOTTOMLEFT:
                case HTBOTTOMRIGHT:
                    return defaultHit;
                default:
                    break;
            }
            POINT clientPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &clientPt);
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            const UINT dpi = GetDpiForWindow(hwnd);
            if (clientPt.y >= clientRect.top && clientPt.y < clientRect.top + TitleBarHeight(dpi)) {
                // The three button rects take priority over the plain
                // draggable band -- returning HTMINBUTTON/HTMAXBUTTON/
                // HTCLOSE here is what makes the WM_NCLBUTTONDOWN handler
                // below know which button a click landed on (these codes
                // are *not* enough on their own to trigger the action --
                // confirmed live that DefWindowProcW's default handling
                // doesn't minimize/maximize/close just because
                // WM_NCHITTEST reports one of these, once the real
                // caption has been shrunk the way this class does; see
                // WM_NCLBUTTONDOWN's own comment). See PaintTitleBar for
                // the purely-visual half of this.
                RECT r = CloseButtonRect(clientRect, dpi);
                if (PtInRect(&r, clientPt)) {
                    return HTCLOSE;
                }
                r = MaximizeButtonRect(clientRect, dpi);
                if (PtInRect(&r, clientPt)) {
                    return HTMAXBUTTON;
                }
                r = MinimizeButtonRect(clientRect, dpi);
                if (PtInRect(&r, clientPt)) {
                    return HTMINBUTTON;
                }
                // The mode-toggle/manage-windows buttons are plain
                // client-area buttons (see their own rect comment in the
                // header) -- HTCLIENT here, not HTCAPTION, is what lets
                // WM_LBUTTONDOWN actually reach WM_LBUTTONDOWN's handler
                // below instead of starting a window drag.
                r = ManageWindowsButtonRect(clientRect, dpi);
                if (PtInRect(&r, clientPt)) {
                    return HTCLIENT;
                }
                r = ModeToggleButtonRect(clientRect, dpi);
                if (PtInRect(&r, clientPt)) {
                    return HTCLIENT;
                }
                if (TileMaximizeButtonVisible()) {
                    r = TileMaximizeButtonRect(clientRect, dpi);
                    if (PtInRect(&r, clientPt)) {
                        return HTCLIENT;
                    }
                }
                r = AlignmentButtonRect(clientRect, dpi);
                if (PtInRect(&r, clientPt)) {
                    return HTCLIENT;
                }
                // The rest of the band is draggable/double-click-to-
                // maximize, same as the native caption it replaces --
                // returning HTCAPTION here is what makes DefWindowProcW's
                // own WM_NCLBUTTONDOWN handling do all of that for free.
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_NCMOUSEMOVE: {
            // wParam is the same hit-test code WM_NCHITTEST just
            // returned for this point -- no need to recompute which
            // button rect the cursor is over from coordinates.
            const std::optional<UINT> newHover =
                (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)
                    ? std::optional<UINT>(static_cast<UINT>(wParam))
                    : std::nullopt;
            if (!trackingNcMouseLeave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme)) {
                    trackingNcMouseLeave_ = true;
                }
            }
            if (newHover != hoveredTitleBarButton_) {
                hoveredTitleBarButton_ = newHover;
                InvalidateTitleBar();
                UpdateTooltip();
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_NCMOUSELEAVE:
            trackingNcMouseLeave_ = false;
            if (hoveredTitleBarButton_.has_value()) {
                hoveredTitleBarButton_.reset();
                InvalidateTitleBar();
                UpdateTooltip();
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_NCLBUTTONDOWN:
            // Committing on *down* here, not up, despite that being the
            // less-native-feeling choice (a real caption button commits
            // on release, letting you drag off to cancel) -- confirmed
            // live, with message-level logging, that WM_NCLBUTTONDOWN
            // with wParam HTMINBUTTON/HTMAXBUTTON/HTCLOSE correctly
            // arrives here, but letting it fall through to
            // DefWindowProcW (as an earlier version of this code did,
            // committing on the *up* message instead) never resulted in
            // a WM_NCLBUTTONUP being delivered back to this window proc
            // at all -- DefWindowProcW's own default handling for these
            // specific hit-test codes appears to enter its own internal
            // capture/tracking loop (the same general mechanism a
            // native caption button's own press-track-release visual
            // feedback relies on) that consumes the eventual release
            // internally rather than dispatching it back to the owning
            // window's own WndProc, and -- per this class's own earlier
            // finding -- doesn't perform the action itself either once
            // the real caption has been shrunk this way. Handling on
            // down is what actually works.
            switch (wParam) {
                case HTMINBUTTON:
                    ShowWindow(hwnd, SW_MINIMIZE);
                    // The window (and its title bar) is about to be
                    // hidden -- explicitly drop the hover state too, not
                    // just repaint, so the tracked tooltip doesn't keep
                    // floating over the desktop where the button used
                    // to be.
                    hoveredTitleBarButton_.reset();
                    UpdateTooltip();
                    return 0;
                case HTMAXBUTTON:
                    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    // Same button, but its meaning just flipped
                    // (Maximize <-> Restore) -- refresh the tooltip text
                    // immediately rather than waiting for the cursor to
                    // leave and re-enter the button.
                    UpdateTooltip();
                    return 0;
                case HTCLOSE:
                    // Same graceful-close path a real caption's close
                    // button would trigger (WM_SYSCOMMAND/SC_CLOSE
                    // ultimately just posts WM_CLOSE) -- reuses the
                    // existing WM_CLOSE handler unchanged (onClosing_
                    // reparents members back out before DefWindowProcW's
                    // own WM_CLOSE handling destroys the window).
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                default:
                    break;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_SETCURSOR: {
            // DefWindowProc routes WM_SETCURSOR to the *parent* first,
            // specifically so a parent can override a child's own cursor
            // decision -- this chrome is the parent of every embedded
            // member, and wParam is whichever window is actually under
            // the cursor (a member's own HWND when it's that member's own
            // frame asking, not this chrome's). A member's resize border
            // is stripped on join (ReparentIntoGroup) but some apps
            // re-apply their own frame styles later (see
            // ReapplyChildFrameStyles), which brings its resize hit-test
            // -- and cursor -- back even though GroupManager already
            // refuses the actual resize (EnforceMemberRect/WM_CANCELMODE
            // in main.cpp). Forced back to a plain arrow here regardless
            // of whether that reapply has been caught yet, so the cursor
            // never lies about what a drag would do. Guarded on
            // wParam != hwnd: the chrome's *own* resize border reports
            // these same codes with wParam == hwnd and must keep its
            // normal resize cursors.
            // Diagnostic probe (Part 2 of the "resize cursor" investigation
            // -- see PLAN.md): last round's override above looked right on
            // paper but the cursor still showed, so rather than guess a
            // third fix, log what this handler is actually being told,
            // whenever it changes for a given window. If lines never
            // appear while hovering a member's edge, this handler is never
            // even reached for that member (it's deciding its own cursor
            // without deferring to DefWindowProc) and no change to this
            // override could ever matter; if lines appear with HTCLIENT
            // rather than a resize code, the member is doing client-area
            // cursor logic of its own, which is why IsResizeHitCode below
            // never matches.
            {
                const HWND cursorTarget = reinterpret_cast<HWND>(wParam);
                const WORD hitCode = LOWORD(lParam);
                if (cursorTarget != lastCursorProbeWindow_ || hitCode != lastCursorProbeHitCode_) {
                    lastCursorProbeWindow_ = cursorTarget;
                    lastCursorProbeHitCode_ = hitCode;
                    LogDebug(std::format(L"[Polish] Cursor: WM_SETCURSOR target={} hitCode={} msg=0x{:x}",
                                          DescribeWindow(cursorTarget), hitCode, HIWORD(lParam)));
                }
            }
            if (reinterpret_cast<HWND>(wParam) != hwnd && IsResizeHitCode(LOWORD(lParam))) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            // Only for hovering a splitter -- everything else (the
            // window's own resize border, etc.) still needs its normal
            // default handling, so only intercept the plain client-area
            // case and only when a splitter is actually under the
            // cursor.
            if (IsTiledMode(mode_) && LOWORD(lParam) == HTCLIENT) {
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
            // Double-buffered: PaintTabStrip issues many separate
            // FillRect/DrawText/RoundRect calls (one active tab alone is
            // several), and drawing those directly to the live screen
            // HDC one at a time -- confirmed via diagnostic logging that
            // a tab switch produces exactly one substantive WM_PAINT for
            // the strip, not a repeated-invalidate storm -- is a classic
            // source of visible flicker even from a single repaint, since
            // DWM/the display can capture a partially-drawn frame.
            // Rendering into an off-screen bitmap first and blitting the
            // finished result in one BitBlt makes every repaint atomic
            // from the screen's point of view.
            const int width = clientRect.right - clientRect.left;
            const int height = clientRect.bottom - clientRect.top;
            if (width > 0 && height > 0) {
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
                HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);
                PaintTabStrip(memDC, clientRect);
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
            {
                // Mode-toggle/manage-windows: checked before the Tile-
                // mode splitter and Tab-mode tab hit-tests below, since
                // the title bar band (and these two buttons within it)
                // exists in both modes. Fires directly on down, same as
                // the caption buttons -- no capture/drag handling needed
                // for a one-shot command button.
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                const UINT dpi = GetDpiForWindow(hwnd);
                RECT r = ManageWindowsButtonRect(clientRect, dpi);
                if (PtInRect(&r, pt)) {
                    if (onEditWindowsRequested_) {
                        onEditWindowsRequested_();
                    }
                    // The picker dialog just took activation -- drop the
                    // hover/tooltip explicitly rather than leaving it
                    // floating over this now-inactive window until the
                    // cursor happens to move.
                    hoveredActionButton_.reset();
                    InvalidateTitleBar();
                    UpdateTooltip();
                    return 0;
                }
                r = ModeToggleButtonRect(clientRect, dpi);
                if (PtInRect(&r, pt)) {
                    if (onModeToggleRequested_) {
                        onModeToggleRequested_();
                    }
                    // onModeToggleRequested_ calls SetMode back
                    // synchronously, so mode_ (and therefore the
                    // tooltip's Switch-to-Tile/Switch-to-Tab text) is
                    // already up to date here.
                    UpdateTooltip();
                    return 0;
                }
                if (TileMaximizeButtonVisible()) {
                    r = TileMaximizeButtonRect(clientRect, dpi);
                    if (PtInRect(&r, pt)) {
                        if (onTileMaximizeToggleRequested_) {
                            onTileMaximizeToggleRequested_();
                        }
                        // Same reasoning as mode-toggle above --
                        // onTileMaximizeToggleRequested_ calls
                        // SetTileMaximized back synchronously.
                        UpdateTooltip();
                        return 0;
                    }
                }
                r = AlignmentButtonRect(clientRect, dpi);
                if (PtInRect(&r, pt)) {
                    if (onAlignmentToggleRequested_) {
                        onAlignmentToggleRequested_();
                    }
                    // Same reasoning as mode-toggle above --
                    // onAlignmentToggleRequested_ calls SetAlignment
                    // back synchronously.
                    UpdateTooltip();
                    return 0;
                }
            }
            if (IsTiledMode(mode_)) {
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
            {
                // Mode-toggle/manage-windows hover -- checked in both
                // modes (the title bar band always exists), independent
                // of the mode-specific hover tracking below.
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                const UINT dpi = GetDpiForWindow(hwnd);
                std::optional<TitleBarActionButton> newActionHover;
                RECT r = ManageWindowsButtonRect(clientRect, dpi);
                if (PtInRect(&r, pt)) {
                    newActionHover = TitleBarActionButton::ManageWindows;
                } else {
                    r = ModeToggleButtonRect(clientRect, dpi);
                    if (PtInRect(&r, pt)) {
                        newActionHover = TitleBarActionButton::ModeToggle;
                    } else {
                        bool hitTileMaximize = false;
                        if (TileMaximizeButtonVisible()) {
                            r = TileMaximizeButtonRect(clientRect, dpi);
                            hitTileMaximize = PtInRect(&r, pt) != FALSE;
                        }
                        if (hitTileMaximize) {
                            newActionHover = TitleBarActionButton::TileMaximize;
                        } else {
                            r = AlignmentButtonRect(clientRect, dpi);
                            if (PtInRect(&r, pt)) {
                                newActionHover = TitleBarActionButton::Alignment;
                            }
                        }
                    }
                }
                if (!trackingMouseLeave_) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    if (TrackMouseEvent(&tme)) {
                        trackingMouseLeave_ = true;
                    }
                }
                if (newActionHover != hoveredActionButton_) {
                    hoveredActionButton_ = newActionHover;
                    InvalidateTitleBar();
                    UpdateTooltip();
                }
            }
            if (IsTiledMode(mode_)) {
                // Splitter hover highlight -- Tile/Stack has no tabs, so
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
            if (hoveredActionButton_.has_value()) {
                hoveredActionButton_.reset();
                InvalidateTitleBar();
                UpdateTooltip();
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
            if (IsTiledMode(mode_)) {
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
            // Minimizing/restoring is handled entirely separately, via
            // onMinimizedChanged_ -- see that callback's own comment for
            // why a plain reflow (onResized_) would be actively wrong for
            // an attached member here. minimized_ tracks the last-seen
            // state so this only fires on the actual transition, not on
            // every WM_SIZE while already minimized (Windows sends more
            // than one) or every ordinary resize once restored.
            if (const bool nowMinimized = (wParam == SIZE_MINIMIZED); nowMinimized != minimized_) {
                minimized_ = nowMinimized;
                if (onMinimizedChanged_) {
                    onMinimizedChanged_(nowMinimized);
                }
                return 0;
            }
            if (minimized_) {
                // Still minimized (e.g. a redundant WM_SIZE) -- nothing
                // to relayout against a degenerate content rect.
                return 0;
            }
            // Members are real children now -- they already move for
            // free when the chrome itself moves (no callback needed for
            // that at all, unlike the old reposition-only design). A
            // *resize* still needs an explicit relayout, since children
            // don't auto-resize to fill a bigger/smaller parent.
            if (onResized_) {
                onResized_();
            }
            return 0;

        case WM_MOVE:
            // Members move for free (see WM_SIZE's own comment) -- this
            // is only for onMoved_'s own use case, the active-tile ring,
            // which is a separate top-level window and does not.
            if (onMoved_) {
                onMoved_();
            }
            return 0;

        case WM_WINDOWPOSCHANGED: {
            // See SetOnZOrderChanged's own comment for why this exists
            // and why WM_WINDOWPOSCHANGED rather than WM_ACTIVATE.
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            if ((pos->flags & SWP_NOZORDER) == 0 && onZOrderChanged_) {
                onZOrderChanged_();
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_PARENTNOTIFY:
            // A member (a real WS_CHILD now) was clicked -- forward the
            // click point so the owner can resolve which member and
            // activate it, a click-based counterpart to focus-based
            // activation for apps that don't move keyboard focus on
            // click/re-activation (see SetOnMemberClicked's own
            // comment). lParam is the click point in *this* window's own
            // client coordinates for every WM_LBUTTONDOWN-family
            // wParam -- not meaningful for WM_PARENTNOTIFY's other two
            // reasons (a child being created/destroyed), which is why
            // this only acts on WM_LBUTTONDOWN specifically.
            if (LOWORD(wParam) == WM_LBUTTONDOWN && onMemberClicked_) {
                const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                onMemberClicked_(pt);
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

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
    if (memberTitles_.empty() || IsTiledMode(mode_)) {
        return rects;  // Tile/Stack has no clickable tabs -- see Show()'s comment
    }
    const UINT dpi = GetDpiForWindow(window_);

    if (alignment_ == GroupAlignment::Vertical) {
        // A left-edge column of fixed width (see kTabStripWidth's own
        // comment for why fixed rather than content-sized), tabs
        // stacked top-to-bottom instead of left-to-right -- otherwise
        // the same leading-inset/gap/min-max-clamp shape as the
        // horizontal case below, just along the other axis (reusing
        // kTabStripLeftPadding/kTabGap as the vertical equivalents of
        // "inset before the first tab" / "gap between tabs", and
        // kTabStripHeight doing double duty as each tab's fixed row
        // height here instead of the strip's own height).
        const int stripLeft = clientRect.left;
        const int stripWidth = TabColumnWidth(dpi);
        const int rowHeight = Scale(kTabStripHeight, dpi);
        const int topPadding = Scale(kTabStripLeftPadding, dpi);
        const int rowGap = Scale(kTabGap, dpi);
        int y = clientRect.top + TitleBarHeight(dpi) + topPadding;
        for (size_t i = 0; i < memberTitles_.size(); ++i) {
            if (y >= clientRect.bottom) {
                break;
            }
            rects.push_back(RECT{stripLeft, y, stripLeft + stripWidth,
                                  std::min(y + rowHeight, static_cast<int>(clientRect.bottom))});
            y += rowHeight + rowGap;
        }
        return rects;
    }

    const int tabTop = clientRect.top + TitleBarHeight(dpi);  // below the custom title bar band
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
            RECT{x, tabTop, std::min(x + tabWidth, static_cast<int>(clientRect.right)), tabTop + tabHeight});
        x += tabWidth + tabGap;
    }
    return rects;
}

void GroupChromeWindow::PaintTabStrip(HDC hdc, const RECT& clientRect) {
    // Windows 11's own tab style (File Explorer, Notepad): a strip the
    // tabs sit in, an active tab that's the *same* color as the content
    // area below it (so it visually merges/"grows out of" the content,
    // no border between them), rounded top corners only. Unselected tabs
    // are deliberately de-emphasized on *both* axes -- background and
    // text -- rather than the active tab being emphasized on top of an
    // otherwise-neutral strip: a lighter/receding fill (in dark mode the
    // active tab is already the lighter of the two, so de-emphasis there
    // falls entirely on text/hover instead) and disabled-looking grey
    // text, never full-strength black/white. Two invariants hold in both
    // themes: unselected sits strictly between the strip and the active
    // tab, so a *hovered* unselected tab can never outshine the selected
    // one (a real, confirmed bug in the old dark-mode numbers, where
    // hover 0x2B was lighter than active 0x20); and unselected text is
    // always a mid-grey "disabled" tone, never the same strength as the
    // active tab's (the old light-mode numbers had this backwards --
    // unselected text was pure 0x000000, stronger than active's 0x1A).
    const bool dark = IsDarkModeEnabled();
    const COLORREF kContentColor = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xFF, 0xFF, 0xFF);
    const COLORREF kActiveTabColor = kContentColor;
    const COLORREF kInactiveTabColor = dark ? RGB(0x0A, 0x0A, 0x0A) : RGB(0xEF, 0xEF, 0xEF);
    const COLORREF kHoverTabColor = dark ? RGB(0x17, 0x17, 0x17) : RGB(0xF7, 0xF7, 0xF7);
    const COLORREF kActiveBorderColor = dark ? RGB(0x3F, 0x3F, 0x3F) : RGB(0xD8, 0xD8, 0xD8);
    const COLORREF kActiveTextColor = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A);
    const COLORREF kInactiveTextColor = dark ? RGB(0x9A, 0x9A, 0x9A) : RGB(0x60, 0x60, 0x60);

    const UINT dpi = GetDpiForWindow(window_);
    const int cornerRadius = Scale(kTabCornerRadius, dpi);
    const int titleBarHeight = TitleBarHeight(dpi);

    // The custom title bar band itself -- reserved in both modes (see
    // HeaderHeight's own comment).
    RECT titleBarRect{clientRect.left, clientRect.top, clientRect.right, clientRect.top + titleBarHeight};
    HBRUSH titleBarBrush = CreateSolidBrush(kContentColor);
    FillRect(hdc, &titleBarRect, titleBarBrush);
    DeleteObject(titleBarBrush);
    PaintTitleBar(hdc, clientRect);

    if (IsTiledMode(mode_)) {
        // No tab strip in Tile/Stack -- there are no tabs to render
        // (every member is simultaneously visible instead, positioned by
        // GroupManager's grid layout). Content fills everything below
        // the title bar band.
        RECT tileContentRect{clientRect.left, clientRect.top + titleBarHeight, clientRect.right, clientRect.bottom};
        HBRUSH contentBrush = CreateSolidBrush(kContentColor);
        FillRect(hdc, &tileContentRect, contentBrush);
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
                RECT bar{clientRect.left + boundary - width / 2, tileContentRect.top,
                         clientRect.left + boundary + (width - width / 2), tileContentRect.bottom};
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
                RECT bar{clientRect.left, tileContentRect.top + boundary - width / 2, clientRect.right,
                         tileContentRect.top + boundary + (width - width / 2)};
                HBRUSH splitterBrush = CreateSolidBrush(highlighted ? accentColor : kActiveBorderColor);
                FillRect(hdc, &bar, splitterBrush);
                DeleteObject(splitterBrush);
            }
        }
        return;
    }

    const int tabHeight = Scale(kTabStripHeight, dpi);
    const int tabColumnWidth = TabColumnWidth(dpi);        // 0 unless Vertical
    const int tabStripLeftWidth = TabStripLeftWidth(dpi);  // 0 unless Vertical

    // Hoisted above the strip/connector painting below (rather than
    // computed just before the tab-label loop, as it used to be) --
    // paintConnectorBorderLine, further down, needs the active tab's own
    // rect to know where to leave a gap for the fillets to bridge.
    // Depends only on clientRect/memberTitles_/mode_/alignment_, none of
    // which the painting below touches, so hoisting it changes nothing
    // about what it returns.
    const std::vector<RECT> tabRects = ComputeTabRects(clientRect);
    // The active tab's own rect, if there is one with a currently valid
    // index -- both paintConnectorBorderLine and the tab-painting loop
    // further down need this, computed once so the two can never
    // disagree about which tab (if any) is active for painting purposes.
    const RECT* activeTabRect = (activeIndex_ < tabRects.size()) ? &tabRects[activeIndex_] : nullptr;

    // Light outline continuing the active tab's own border pen and the
    // concave fillets (see kActiveBorderThickness's own comment) out to
    // both ends of the header, along the strip's trailing edge -- so the
    // three read as one unbroken line separating the tab strip from the
    // merged active-tab/connector shape, interrupted only where the
    // fillets bridge it around the active tab (or not interrupted at
    // all, when there isn't one). A closure, not a free function: called
    // from two places below (the tabRects.empty() early-out and after
    // the tab-painting loop), and it needs enough of this function's own
    // locals (hdc, dpi-derived sizes, activeTabRect) that threading them
    // all through as parameters would be noisier than capturing them.
    // Deliberately called *after* every tab has painted its own fill --
    // an inactive tab's fill happens to match the strip's own base color
    // exactly and would otherwise paint straight over this row, erasing
    // it, if this ran first.
    auto paintConnectorBorderLine = [&]() {
        HBRUSH borderBrush = CreateSolidBrush(kActiveBorderColor);
        if (alignment_ == GroupAlignment::Vertical) {
            const int lineRight = clientRect.left + tabColumnWidth;
            const int lineLeft = lineRight - kActiveBorderThickness;
            const int top = clientRect.top + titleBarHeight;
            if (activeTabRect != nullptr) {
                const int gapTop = activeTabRect->top - cornerRadius;
                const int gapBottom = activeTabRect->bottom + cornerRadius;
                RECT above{lineLeft, top, lineRight, gapTop};
                FillRect(hdc, &above, borderBrush);
                RECT below{lineLeft, gapBottom, lineRight, clientRect.bottom};
                FillRect(hdc, &below, borderBrush);
            } else {
                RECT full{lineLeft, top, lineRight, clientRect.bottom};
                FillRect(hdc, &full, borderBrush);
            }
        } else {
            const int lineBottom = clientRect.top + titleBarHeight + tabHeight;
            const int lineTop = lineBottom - kActiveBorderThickness;
            if (activeTabRect != nullptr) {
                const int gapLeft = activeTabRect->left - cornerRadius;
                const int gapRight = activeTabRect->right + cornerRadius;
                RECT left{clientRect.left, lineTop, gapLeft, lineBottom};
                FillRect(hdc, &left, borderBrush);
                RECT right{gapRight, lineTop, clientRect.right, lineBottom};
                FillRect(hdc, &right, borderBrush);
            } else {
                RECT full{clientRect.left, lineTop, clientRect.right, lineBottom};
                FillRect(hdc, &full, borderBrush);
            }
        }
        DeleteObject(borderBrush);
    };

    if (alignment_ == GroupAlignment::Vertical) {
        // Left-column counterpart of the horizontal strip's own base
        // fill below -- same "leftover space reads as more inactive
        // tab, not a visually distinct empty band" reasoning, along the
        // column instead of the row. Active/hover tabs draw their own
        // fill on top, same as the horizontal strip.
        RECT columnRect{clientRect.left, clientRect.top + titleBarHeight, clientRect.left + tabColumnWidth,
                         clientRect.bottom};
        HBRUSH columnBrush = CreateSolidBrush(kInactiveTabColor);
        FillRect(hdc, &columnRect, columnBrush);
        DeleteObject(columnBrush);

        // Vertical counterpart of the horizontal connector band below --
        // a permanent full-height column, colored to match the active
        // tab, between the tab column and the member's own content (see
        // kTabConnectorThickness's comment). Straight edges top and
        // bottom for the same reason the horizontal band's are straight
        // left and right -- only the tab-to-column join itself
        // (DrawConcaveFillet, below) needs a curve.
        if (mode_ == GroupMode::Tab) {
            RECT connectorRect{clientRect.left + tabColumnWidth, clientRect.top + titleBarHeight, clientRect.left + tabStripLeftWidth,
                                clientRect.bottom};
            HBRUSH connectorBrush = CreateSolidBrush(kActiveTabColor);
            FillRect(hdc, &connectorRect, connectorBrush);
            DeleteObject(connectorBrush);
        }
    } else {
        // The strip's own base fill is the inactive-tab color, not a
        // separate "strip background" color -- so the left padding
        // before the first tab, the gaps between tabs, and any leftover
        // space past the last tab all read as "more inactive tab"
        // instead of a visually distinct empty band. Active/hover tabs
        // simply draw their own fill on top.
        RECT stripRect{clientRect.left, clientRect.top + titleBarHeight, clientRect.right,
                        clientRect.top + titleBarHeight + tabHeight};
        HBRUSH stripBrush = CreateSolidBrush(kInactiveTabColor);
        FillRect(hdc, &stripRect, stripBrush);
        DeleteObject(stripBrush);

        // Tab mode only: a permanent full-width band, colored to match
        // the active tab, between the strip and the member's own
        // content -- File Explorer's own command-bar area does the
        // same thing (see kTabConnectorThickness's comment). Straight
        // edges all the way to the window's own left/right edges --
        // rounding those corners (tried first) left an unpainted notch
        // outside the round arc but inside the clipped rect, showing
        // whatever stale content was underneath (a real, confirmed
        // white artifact at the window's edges). Only the tab-to-band
        // join itself (DrawConcaveFillet, below) needs a curve.
        if (mode_ == GroupMode::Tab) {
            RECT bandRect{clientRect.left, stripRect.bottom, clientRect.right,
                           stripRect.bottom + Scale(kTabConnectorThickness, dpi)};
            HBRUSH bandBrush = CreateSolidBrush(kActiveTabColor);
            FillRect(hdc, &bandRect, bandBrush);
            DeleteObject(bandBrush);
        }
    }

    RECT contentRect{clientRect.left + tabStripLeftWidth, clientRect.top + HeaderHeight(dpi), clientRect.right,
                      clientRect.bottom};
    HBRUSH contentBrush = CreateSolidBrush(kContentColor);
    FillRect(hdc, &contentRect, contentBrush);
    DeleteObject(contentBrush);

    // MakeUiFont, not GetStockObject(DEFAULT_GUI_FONT) -- the stock font
    // is fixed at a pre-DPI-awareness size, which left tab labels
    // visibly tiny at high DPI even though every other tab-strip
    // dimension here was already being scaled (confirmed,
    // human-reported). Owned, so both exits below have to delete it --
    // unlike the stock font it replaced, which must never be deleted.
    HFONT font = MakeUiFont(dpi);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    if (tabRects.empty()) {
        // No active tab either (activeTabRect is null whenever tabRects
        // is), so this draws one unbroken line the full length of the
        // header -- nothing for the fillets to bridge around.
        paintConnectorBorderLine();
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        return;
    }

    const int iconSize = Scale(16, dpi);
    const int iconTextGap = Scale(4, dpi);

    // The active tab's label is bold, matching File Explorer/Notepad --
    // inactive tabs keep the regular weight already selected into hdc.
    HFONT boldFont = MakeUiFontWithWeight(dpi, FW_BOLD);

    for (size_t i = 0; i < tabRects.size(); ++i) {
        const RECT& tabRect = tabRects[i];
        const bool active = (i == activeIndex_);
        const bool hovered = !active && hoveredTabIndex_.has_value() && *hoveredTabIndex_ == i;

        {
            const COLORREF fill = active ? kActiveTabColor : (hovered ? kHoverTabColor : kInactiveTabColor);
            HBRUSH tabBrush = CreateSolidBrush(fill);
            HGDIOBJ oldBrush = SelectObject(hdc, tabBrush);
            HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
            HGDIOBJ oldPen = SelectObject(hdc, nullPen);
            // Rounded top corners only (Horizontal) / rounded left
            // corners only (Vertical): RoundRect rounds all four
            // corners of whatever rect it's given, so the *other* two
            // corners are pushed outside the visible tab before
            // drawing, then clipped back to tabRect's real bounds --
            // draws past the clip and gets cut off cleanly, rather than
            // needing a custom two-corners-only-rounded path. Without
            // the clip, a non-active tab's fill color -- unlike the
            // active tab's, which matches the content area exactly --
            // would visibly bleed a sliver into the content area
            // alongside it. Vertical rounds the *left* edge -- flush
            // against the window's own border -- and leaves the right
            // edge (where the tab borders the connector column) square
            // and unbordered, the same relationship Horizontal's
            // top-rounded/bottom-square tabs have to the connector band
            // below them: the content-facing edge is always the square,
            // pen-free one, so the active tab merges into the content
            // rather than reading as a bordered pill beside it.
            //
            // Fill only, no pen, here -- unconditionally, even for the
            // active tab (see the second, border-only pass just below
            // for why its outline is no longer drawn in this same call).
            IntersectClipRect(hdc, tabRect.left, tabRect.top, tabRect.right, tabRect.bottom);
            if (alignment_ == GroupAlignment::Vertical) {
                RoundRect(hdc, tabRect.left, tabRect.top, tabRect.right + cornerRadius, tabRect.bottom, cornerRadius,
                          cornerRadius);
            } else {
                RoundRect(hdc, tabRect.left, tabRect.top, tabRect.right, tabRect.bottom + cornerRadius, cornerRadius,
                          cornerRadius);
            }
            SelectClipRgn(hdc, nullptr);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(tabBrush);

            if (active) {
                // Second pass, border only (NULL_BRUSH, real pen) --
                // splitting this out from the fill pass above is what
                // stops the outline forking at the content-facing
                // corners. A single RoundRect call with both a fill and
                // a pen draws the pen the *entire* length of every edge
                // inside the clip, including the last cornerRadius px of
                // the content-facing side/edge where the fillet arc
                // (DrawConcaveFillet, below) is about to take over --
                // producing two visible lines side by side there (the
                // straight pen plus the curve) instead of one turning
                // into the other. Clipping the border pass short of
                // that corner -- rather than the fillet somehow erasing
                // the overshoot afterward -- is what makes this
                // deterministic regardless of paint order.
                //
                // The `+ kActiveBorderThickness` below lands the pen's
                // last row/column exactly on the arc's own inner
                // endpoint rather than one pixel short of it; nudge this
                // first if the join looks notched or doubled-up live.
                HPEN borderPen = CreatePen(PS_SOLID, kActiveBorderThickness, kActiveBorderColor);
                HGDIOBJ oldBorderPen = SelectObject(hdc, borderPen);
                HGDIOBJ oldBorderBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                if (alignment_ == GroupAlignment::Vertical) {
                    const int clipRight = tabRect.right - cornerRadius + kActiveBorderThickness;
                    IntersectClipRect(hdc, tabRect.left, tabRect.top, clipRight, tabRect.bottom);
                    RoundRect(hdc, tabRect.left, tabRect.top, tabRect.right + cornerRadius, tabRect.bottom,
                              cornerRadius, cornerRadius);
                } else {
                    const int clipBottom = tabRect.bottom - cornerRadius + kActiveBorderThickness;
                    IntersectClipRect(hdc, tabRect.left, tabRect.top, tabRect.right, clipBottom);
                    RoundRect(hdc, tabRect.left, tabRect.top, tabRect.right, tabRect.bottom + cornerRadius,
                              cornerRadius, cornerRadius);
                }
                SelectClipRgn(hdc, nullptr);
                SelectObject(hdc, oldBorderBrush);
                SelectObject(hdc, oldBorderPen);
                DeleteObject(borderPen);
            }
        }

        if (active) {
            // Bridges the active tab into the connector band/column
            // beside it (painted earlier, above) with the same concave
            // join File Explorer uses, instead of the sharp corner a
            // plain rectangle-meets-rectangle join would leave. Also
            // covers the active tab's own border-pen line at that seam
            // (drawn above, clipped exactly to the content-facing edge).
            // Horizontal bridges the tab's bottom-left/bottom-right
            // corners into the band below; Vertical is the same idea
            // rotated 90 degrees -- it bridges the tab's top-right/
            // bottom-right corners into the column to its right (the
            // mirror image, since Vertical's content-facing edge is the
            // right one, not the bottom one).
            if (alignment_ == GroupAlignment::Vertical) {
                const RECT topSquare{tabRect.right - cornerRadius, tabRect.top - cornerRadius, tabRect.right,
                                     tabRect.top};
                DrawConcaveFillet(hdc, topSquare, POINT{topSquare.left, topSquare.top}, kActiveTabColor,
                                   kInactiveTabColor, kActiveBorderColor, kActiveBorderThickness);
                const RECT bottomSquare{tabRect.right - cornerRadius, tabRect.bottom, tabRect.right,
                                        tabRect.bottom + cornerRadius};
                DrawConcaveFillet(hdc, bottomSquare, POINT{bottomSquare.left, bottomSquare.bottom}, kActiveTabColor,
                                   kInactiveTabColor, kActiveBorderColor, kActiveBorderThickness);
            } else {
                const RECT leftSquare{tabRect.left - cornerRadius, tabRect.bottom - cornerRadius, tabRect.left,
                                      tabRect.bottom};
                DrawConcaveFillet(hdc, leftSquare, POINT{leftSquare.left, leftSquare.top}, kActiveTabColor,
                                   kInactiveTabColor, kActiveBorderColor, kActiveBorderThickness);
                const RECT rightSquare{tabRect.right, tabRect.bottom - cornerRadius, tabRect.right + cornerRadius,
                                       tabRect.bottom};
                DrawConcaveFillet(hdc, rightSquare, POINT{rightSquare.right, rightSquare.top}, kActiveTabColor,
                                   kInactiveTabColor, kActiveBorderColor, kActiveBorderThickness);
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
    // After every tab -- see paintConnectorBorderLine's own comment on
    // why painting order matters here.
    paintConnectorBorderLine();
    SelectObject(hdc, oldFont);
    DeleteObject(boldFont);
    DeleteObject(font);
}

int GroupChromeWindow::HeaderHeight(UINT dpi) const {
    // The title bar band is reserved in every mode -- it's now the only
    // place the group's name and window controls are shown at all (see
    // TitleBarHeight's own comment). Tile/Stack have no tab strip beyond
    // that (nothing to click, every member is simultaneously visible).
    // Vertical alignment's tab strip also adds nothing *here* -- it
    // reserves a left column instead (see TabStripLeftWidth), not extra
    // top height, so this is the same TitleBarHeight-only case as
    // Tile/Stack.
    if (mode_ != GroupMode::Tab || alignment_ == GroupAlignment::Vertical) {
        return TitleBarHeight(dpi);
    }
    return TitleBarHeight(dpi) + Scale(kTabStripHeight, dpi) + Scale(kTabConnectorThickness, dpi);
}

int GroupChromeWindow::TitleBarHeight(UINT dpi) const { return Scale(kTitleBarHeight, dpi); }

RECT GroupChromeWindow::CloseButtonRect(const RECT& clientRect, UINT dpi) const {
    const int w = Scale(kTitleBarButtonWidth, dpi);
    const int h = TitleBarHeight(dpi);
    return RECT{clientRect.right - w, clientRect.top, clientRect.right, clientRect.top + h};
}

RECT GroupChromeWindow::MaximizeButtonRect(const RECT& clientRect, UINT dpi) const {
    const RECT close = CloseButtonRect(clientRect, dpi);
    const int w = Scale(kTitleBarButtonWidth, dpi);
    return RECT{close.left - w, close.top, close.left, close.bottom};
}

RECT GroupChromeWindow::MinimizeButtonRect(const RECT& clientRect, UINT dpi) const {
    const RECT maximize = MaximizeButtonRect(clientRect, dpi);
    const int w = Scale(kTitleBarButtonWidth, dpi);
    return RECT{maximize.left - w, maximize.top, maximize.left, maximize.bottom};
}

RECT GroupChromeWindow::ManageWindowsButtonRect(const RECT& clientRect, UINT dpi) const {
    const RECT minimize = MinimizeButtonRect(clientRect, dpi);
    const int w = Scale(kTitleBarButtonWidth, dpi);
    // A full button-width gap (not just the separator line drawn inside
    // it -- see PaintTitleBar) between this button and the caption
    // buttons, so the two clusters read as obviously separate groups
    // rather than five buttons in a row.
    const int gap = Scale(kTitleBarButtonWidth, dpi);
    return RECT{minimize.left - gap - w, minimize.top, minimize.left - gap, minimize.bottom};
}

// Left-to-right order (right to left in this anchor chain, each button
// immediately left of the last): tile-maximize, mode, alignment,
// manage-windows | gap | minimize, maximize, close. TileMaximize is the
// one conditionally-visible button (see TileMaximizeButtonVisible), so
// it anchors the *leftmost* end of the chain instead of sitting in the
// middle of it -- hiding/showing it then only ever changes the title
// text's own right boundary (PaintTitleBar), never shifts any other
// button.
RECT GroupChromeWindow::AlignmentButtonRect(const RECT& clientRect, UINT dpi) const {
    const RECT manageWindows = ManageWindowsButtonRect(clientRect, dpi);
    const int w = Scale(kTitleBarButtonWidth, dpi);
    return RECT{manageWindows.left - w, manageWindows.top, manageWindows.left, manageWindows.bottom};
}

RECT GroupChromeWindow::ModeToggleButtonRect(const RECT& clientRect, UINT dpi) const {
    const RECT alignment = AlignmentButtonRect(clientRect, dpi);
    const int w = Scale(kTitleBarButtonWidth, dpi);
    return RECT{alignment.left - w, alignment.top, alignment.left, alignment.bottom};
}

bool GroupChromeWindow::TileMaximizeButtonVisible() const {
    // A single tile already fills the whole content area on its own --
    // "maximized" wouldn't mean anything different from the normal
    // state, so there's nothing for this button to do outside a tiled
    // mode (Tile or Stack) with 2+ members. Mirrors GroupState's own
    // SetTileMaximized-reset condition (see its comment) so the button
    // is never shown in a state IsTileMaximized() itself would refuse
    // to stay true in.
    return IsTiledMode(mode_) && memberTitles_.size() > 1;
}

RECT GroupChromeWindow::TileMaximizeButtonRect(const RECT& clientRect, UINT dpi) const {
    const RECT modeToggle = ModeToggleButtonRect(clientRect, dpi);
    const int w = Scale(kTitleBarButtonWidth, dpi);
    return RECT{modeToggle.left - w, modeToggle.top, modeToggle.left, modeToggle.bottom};
}

int GroupChromeWindow::TabColumnWidth(UINT dpi) const {
    if (mode_ != GroupMode::Tab || alignment_ != GroupAlignment::Vertical) {
        return 0;
    }
    return Scale(kTabStripWidth, dpi);
}

int GroupChromeWindow::TabStripLeftWidth(UINT dpi) const {
    const int columnWidth = TabColumnWidth(dpi);
    if (columnWidth == 0) {
        return 0;
    }
    return columnWidth + Scale(kTabConnectorThickness, dpi);
}

void GroupChromeWindow::PaintTitleBar(HDC hdc, const RECT& clientRect) const {
    const bool dark = IsDarkModeEnabled();
    const UINT dpi = GetDpiForWindow(window_);
    const int titleBarHeight = TitleBarHeight(dpi);

    // Icon + title text, left-aligned -- the title text is whatever the
    // owner already set via SetWindowTextW (main.cpp sets this to the
    // group's name, same as it always has -- that call still matters for
    // the taskbar/Alt+Tab tooltip text even though the native caption no
    // longer paints it itself). Read fresh every paint rather than cached,
    // matching this class's existing SetMemberTitles/SetMemberIcons
    // convention of not needing a separate invalidation path when the
    // owner updates it.
    const int iconSize = Scale(kTitleBarIconSize, dpi);
    const int iconLeft = clientRect.left + Scale(kTitleBarIconLeftPadding, dpi);
    const int iconTop = clientRect.top + (titleBarHeight - iconSize) / 2;
    // GCLP_HICONSM -- the same small icon set once at class-registration
    // time (see the constructor), not a fresh load; borrowed, not owned.
    const HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(window_, GCLP_HICONSM));
    if (icon != nullptr) {
        DrawIconEx(hdc, iconLeft, iconTop, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    }

    wchar_t title[256] = L"";
    GetWindowTextW(window_, title, static_cast<int>(sizeof(title) / sizeof(title[0])));

    // The real system caption font (NONCLIENTMETRICS' lfCaptionFont) --
    // this class is replacing the native caption, so using the exact
    // font Windows itself would have used here (rather than the tab
    // strip's own DEFAULT_GUI_FONT/bold-DEFAULT_GUI_FONT pair) is what
    // makes the custom band actually read as a title bar.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT captionFont = CreateFontIndirectW(&metrics.lfCaptionFont);
    HGDIOBJ oldFont = SelectObject(hdc, captionFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A));

    const int textLeft = (icon != nullptr) ? iconLeft + iconSize + Scale(kTitleBarIconTextGap, dpi) : iconLeft;
    // TileMaximize is the leftmost of the four action buttons when
    // visible (see the anchor-chain comment above AlignmentButtonRect);
    // otherwise ModeToggle is. This is the one place that still needs
    // its own visibility check, now that TileMaximizeButtonRect no
    // longer shifts anything to its right when it disappears.
    const int textRight =
        (TileMaximizeButtonVisible() ? TileMaximizeButtonRect(clientRect, dpi) : ModeToggleButtonRect(clientRect, dpi))
            .left;
    RECT textRect{textLeft, clientRect.top, textRight, clientRect.top + titleBarHeight};
    DrawTextW(hdc, title, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
    DeleteObject(captionFont);

    // The three caption buttons -- glyphs only drawn in the plain
    // "native Windows glyph shapes" convention already established in
    // AltTabListWindow.cpp (a single line for minimize, a square/
    // overlapping-squares outline for maximize/restore, an X for close),
    // not new iconography. This is purely the visual half -- the actual
    // minimize/maximize/close action happens in WM_NCLBUTTONDOWN, keyed
    // off the same HTMINBUTTON/HTMAXBUTTON/HTCLOSE codes WM_NCHITTEST
    // reports for these same rects.
    const RECT minimizeRect = MinimizeButtonRect(clientRect, dpi);
    const RECT maximizeRect = MaximizeButtonRect(clientRect, dpi);
    const RECT closeRect = CloseButtonRect(clientRect, dpi);
    const COLORREF hoverFill = dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xE5, 0xE5, 0xE5);
    const COLORREF glyphColor = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A);

    auto fillIfHovered = [&](const RECT& rect, UINT hitTest, bool isClose) {
        if (hoveredTitleBarButton_ != hitTest) {
            return false;
        }
        HBRUSH brush = CreateSolidBrush(isClose ? kTitleBarCloseHoverColor : hoverFill);
        FillRect(hdc, &rect, brush);
        DeleteObject(brush);
        return true;
    };
    fillIfHovered(minimizeRect, HTMINBUTTON, false);
    const bool maximizeHovered = fillIfHovered(maximizeRect, HTMAXBUTTON, false);
    const bool closeHovered = fillIfHovered(closeRect, HTCLOSE, true);

    HPEN glyphPen = CreatePen(PS_SOLID, 1, closeHovered ? RGB(0xFF, 0xFF, 0xFF) : glyphColor);
    HGDIOBJ oldPen = SelectObject(hdc, glyphPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    auto drawLine = [hdc](int x1, int y1, int x2, int y2) {
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
    };

    // Fixed-size glyph, centered within a button rect regardless of how
    // big the button's own clickable hit-box is (see kTitleBarGlyphSize's
    // comment for why -- deriving the glyph from the button box size was
    // the actual bug behind the "huge buttons" report).
    const int glyphSize = Scale(kTitleBarGlyphSize, dpi);
    auto glyphRect = [glyphSize](const RECT& buttonRect) {
        const int cx = (buttonRect.left + buttonRect.right) / 2;
        const int cy = (buttonRect.top + buttonRect.bottom) / 2;
        const int half = glyphSize / 2;
        return RECT{cx - half, cy - half, cx + half, cy + half};
    };

    const RECT minimizeGlyph = glyphRect(minimizeRect);
    drawLine(minimizeGlyph.left, minimizeGlyph.bottom, minimizeGlyph.right, minimizeGlyph.bottom);

    const RECT maximizeGlyph = glyphRect(maximizeRect);
    if (IsZoomed(window_)) {
        // Restore glyph: two overlapping offset squares, the front one
        // filled with whatever's already behind it (the hover fill, or
        // the band's own background if not hovered) before being
        // outlined, so it properly occludes the back square underneath
        // -- same technique/reasoning as AltTabListWindow's identical
        // restore glyph (plain NULL_BRUSH outlines would just show both
        // squares' lines crossing through each other instead).
        const int offset = Scale(kTitleBarRestoreGlyphOffset, dpi);
        Rectangle(hdc, maximizeGlyph.left + offset, maximizeGlyph.top, maximizeGlyph.right,
                  maximizeGlyph.bottom - offset);
        HBRUSH occludeBrush = CreateSolidBrush(maximizeHovered ? hoverFill : (dark ? RGB(0x20, 0x20, 0x20) : RGB(0xFF, 0xFF, 0xFF)));
        SelectObject(hdc, occludeBrush);
        Rectangle(hdc, maximizeGlyph.left, maximizeGlyph.top + offset, maximizeGlyph.right - offset,
                  maximizeGlyph.bottom);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        DeleteObject(occludeBrush);
    } else {
        Rectangle(hdc, maximizeGlyph.left, maximizeGlyph.top, maximizeGlyph.right, maximizeGlyph.bottom);
    }

    const RECT closeGlyph = glyphRect(closeRect);
    drawLine(closeGlyph.left, closeGlyph.top, closeGlyph.right, closeGlyph.bottom);
    drawLine(closeGlyph.right, closeGlyph.top, closeGlyph.left, closeGlyph.bottom);

    // A thin separator centered in the full button-width gap between the
    // mode-toggle/manage-windows buttons and the min/max/close caption
    // buttons (see ManageWindowsButtonRect's own comment on that gap),
    // so the two are visibly distinct groups rather than reading as five
    // undifferentiated buttons in a row. PS_SOLID width 1 is a cosmetic
    // pen (always exactly 1 physical device pixel regardless of DPI),
    // the same hairline convention the active tab's own border pen uses.
    {
        const int separatorInset = Scale(kTitleBarSeparatorInset, dpi);
        const int separatorX = (ManageWindowsButtonRect(clientRect, dpi).right + minimizeRect.left) / 2;
        const COLORREF separatorColor = dark ? RGB(0x45, 0x45, 0x45) : RGB(0xD5, 0xD5, 0xD5);
        HPEN separatorPen = CreatePen(PS_SOLID, 1, separatorColor);
        SelectObject(hdc, separatorPen);
        drawLine(separatorX, clientRect.top + separatorInset, separatorX,
                 clientRect.top + titleBarHeight - separatorInset);
        SelectObject(hdc, glyphPen);  // deselect separatorPen before deleting it
        DeleteObject(separatorPen);
    }

    // Mode-toggle and manage-windows: two more buttons, same rect/hover/
    // glyph machinery as the three above, but plain client-area buttons
    // (see their rect comments) with an ordinary hover fill -- no
    // close-red special case, so a fresh plain-glyphColor pen rather
    // than reusing glyphPen (which may currently be the close button's
    // hover-white).
    HPEN actionGlyphPen = CreatePen(PS_SOLID, 1, glyphColor);
    SelectObject(hdc, actionGlyphPen);

    const RECT manageWindowsRect = ManageWindowsButtonRect(clientRect, dpi);
    const bool manageWindowsHovered = hoveredActionButton_ == TitleBarActionButton::ManageWindows;
    if (manageWindowsHovered) {
        HBRUSH brush = CreateSolidBrush(hoverFill);
        FillRect(hdc, &manageWindowsRect, brush);
        DeleteObject(brush);
    }
    // "Manage windows" glyph: three horizontal lines -- the same list-
    // icon convention as a hamburger menu, reading as "this group's
    // window list" without inventing new iconography beyond this file's
    // existing line/rectangle vocabulary.
    const RECT manageWindowsGlyph = glyphRect(manageWindowsRect);
    // Tighter vertical spacing than the full glyph box -- confirmed,
    // human-reported, that stretching the three lines across the whole
    // glyph height read as "too vertical" for a hamburger icon; this
    // matches a more typical hamburger's tighter, more compact spacing.
    const int hamburgerHeight = (manageWindowsGlyph.bottom - manageWindowsGlyph.top) * 3 / 5;
    const int hamburgerTop =
        manageWindowsGlyph.top + (manageWindowsGlyph.bottom - manageWindowsGlyph.top - hamburgerHeight) / 2;
    const int manageWindowsStep = hamburgerHeight / 2;
    for (int i = 0; i <= 2; ++i) {
        const int y = hamburgerTop + i * manageWindowsStep;
        drawLine(manageWindowsGlyph.left, y, manageWindowsGlyph.right, y);
    }

    const RECT modeToggleRect = ModeToggleButtonRect(clientRect, dpi);
    const bool modeToggleHovered = hoveredActionButton_ == TitleBarActionButton::ModeToggle;
    if (modeToggleHovered) {
        HBRUSH brush = CreateSolidBrush(hoverFill);
        FillRect(hdc, &modeToggleRect, brush);
        DeleteObject(brush);
    }
    const RECT modeToggleGlyph = glyphRect(modeToggleRect);
    if (mode_ == GroupMode::Tile) {
        // Tile-mode glyph: a 2x2 grid of small squares -- depicts the
        // *current* layout, the same convention a view-mode toggle
        // button normally uses (e.g. Explorer's list/grid switcher),
        // rather than the mode a click would switch to.
        const int cellW = (modeToggleGlyph.right - modeToggleGlyph.left - 2) / 2;
        const int cellH = (modeToggleGlyph.bottom - modeToggleGlyph.top - 2) / 2;
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const int left = modeToggleGlyph.left + col * (cellW + 2);
                const int top = modeToggleGlyph.top + row * (cellH + 2);
                Rectangle(hdc, left, top, left + cellW, top + cellH);
            }
        }
    } else if (mode_ == GroupMode::Stack) {
        // Stack-mode glyph: three small bars laid out along the stack's
        // own axis -- three side-by-side vertical bars for Horizontal
        // (a single row of tiles), three stacked horizontal bars for
        // Vertical (a single column) -- same "depicts the current
        // layout" convention as Tile's 2x2 grid above.
        constexpr int kBarGap = 2;
        if (alignment_ == GroupAlignment::Vertical) {
            const int barHeight = (modeToggleGlyph.bottom - modeToggleGlyph.top - 2 * kBarGap) / 3;
            for (int i = 0; i < 3; ++i) {
                const int top = modeToggleGlyph.top + i * (barHeight + kBarGap);
                Rectangle(hdc, modeToggleGlyph.left, top, modeToggleGlyph.right, top + barHeight);
            }
        } else {
            const int barWidth = (modeToggleGlyph.right - modeToggleGlyph.left - 2 * kBarGap) / 3;
            for (int i = 0; i < 3; ++i) {
                const int left = modeToggleGlyph.left + i * (barWidth + kBarGap);
                Rectangle(hdc, left, modeToggleGlyph.top, left + barWidth, modeToggleGlyph.bottom);
            }
        }
    } else {
        // Tab-mode glyph: a small tab notch overlapping the top-left of
        // a page outline -- same overlapping-rectangle occlusion
        // technique as the restore glyph above (fill the front shape
        // with the background color before outlining it), so the notch
        // reads as sitting in front of the page instead of just two
        // crossing outlines.
        const int notchWidth = (modeToggleGlyph.right - modeToggleGlyph.left) * 2 / 3;
        const int notchHeight = (modeToggleGlyph.bottom - modeToggleGlyph.top) / 3;
        Rectangle(hdc, modeToggleGlyph.left, modeToggleGlyph.top + notchHeight, modeToggleGlyph.right,
                  modeToggleGlyph.bottom);
        HBRUSH occludeBrush =
            CreateSolidBrush(modeToggleHovered ? hoverFill : (dark ? RGB(0x20, 0x20, 0x20) : RGB(0xFF, 0xFF, 0xFF)));
        SelectObject(hdc, occludeBrush);
        Rectangle(hdc, modeToggleGlyph.left, modeToggleGlyph.top, modeToggleGlyph.left + notchWidth,
                  modeToggleGlyph.top + notchHeight + 1);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        DeleteObject(occludeBrush);
    }

    if (TileMaximizeButtonVisible()) {
        // Same square / overlapping-squares glyph shapes as the window-
        // level maximize/restore caption button above -- not new
        // iconography; this button's position (left of the other two
        // action buttons) and tooltip text ("Maximize tile"/"Restore
        // tile") are what disambiguate it from the window-level one,
        // the same way this file already reuses one glyph vocabulary
        // across every button rather than inventing a shape per action.
        const RECT tileMaximizeRect = TileMaximizeButtonRect(clientRect, dpi);
        const bool tileMaximizeHovered = hoveredActionButton_ == TitleBarActionButton::TileMaximize;
        if (tileMaximizeHovered) {
            HBRUSH brush = CreateSolidBrush(hoverFill);
            FillRect(hdc, &tileMaximizeRect, brush);
            DeleteObject(brush);
        }
        const RECT tileMaximizeGlyph = glyphRect(tileMaximizeRect);
        if (tileMaximized_) {
            const int offset = Scale(kTitleBarRestoreGlyphOffset, dpi);
            Rectangle(hdc, tileMaximizeGlyph.left + offset, tileMaximizeGlyph.top, tileMaximizeGlyph.right,
                      tileMaximizeGlyph.bottom - offset);
            HBRUSH occludeBrush = CreateSolidBrush(tileMaximizeHovered ? hoverFill
                                                                        : (dark ? RGB(0x20, 0x20, 0x20)
                                                                                : RGB(0xFF, 0xFF, 0xFF)));
            SelectObject(hdc, occludeBrush);
            Rectangle(hdc, tileMaximizeGlyph.left, tileMaximizeGlyph.top + offset, tileMaximizeGlyph.right - offset,
                      tileMaximizeGlyph.bottom);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            DeleteObject(occludeBrush);
        } else {
            Rectangle(hdc, tileMaximizeGlyph.left, tileMaximizeGlyph.top, tileMaximizeGlyph.right,
                      tileMaximizeGlyph.bottom);
        }
    }

    {
        // Alignment glyph depends on mode, since what this button
        // actually reorients differs: in Tab mode, a small rectangle
        // outline with a thin filled bar along whichever edge the tab
        // strip currently occupies -- top for Horizontal, left for
        // Vertical. Tile/Stack have no tab strip to depict, so instead
        // these show the rectangle split by divider line(s) matching
        // the grid's own bias -- a vertical divider (side-by-side
        // halves) for Horizontal's wide-biased grid, a horizontal
        // divider (stacked halves) for Vertical's tall-biased grid.
        // Stack draws two dividers along that same axis instead of one
        // -- reading as "many" (1xN) rather than Tile's single split --
        // reusing this file's existing line/rectangle vocabulary rather
        // than inventing new iconography, same as every other button
        // here.
        const RECT alignmentRect = AlignmentButtonRect(clientRect, dpi);
        const bool alignmentHovered = hoveredActionButton_ == TitleBarActionButton::Alignment;
        if (alignmentHovered) {
            HBRUSH brush = CreateSolidBrush(hoverFill);
            FillRect(hdc, &alignmentRect, brush);
            DeleteObject(brush);
        }
        const RECT alignmentGlyph = glyphRect(alignmentRect);
        Rectangle(hdc, alignmentGlyph.left, alignmentGlyph.top, alignmentGlyph.right, alignmentGlyph.bottom);
        if (mode_ == GroupMode::Tile) {
            if (alignment_ == GroupAlignment::Vertical) {
                const int midY = (alignmentGlyph.top + alignmentGlyph.bottom) / 2;
                MoveToEx(hdc, alignmentGlyph.left, midY, nullptr);
                LineTo(hdc, alignmentGlyph.right, midY);
            } else {
                const int midX = (alignmentGlyph.left + alignmentGlyph.right) / 2;
                MoveToEx(hdc, midX, alignmentGlyph.top, nullptr);
                LineTo(hdc, midX, alignmentGlyph.bottom);
            }
        } else if (mode_ == GroupMode::Stack) {
            if (alignment_ == GroupAlignment::Vertical) {
                const int step = (alignmentGlyph.bottom - alignmentGlyph.top) / 3;
                for (int i = 1; i <= 2; ++i) {
                    const int y = alignmentGlyph.top + i * step;
                    MoveToEx(hdc, alignmentGlyph.left, y, nullptr);
                    LineTo(hdc, alignmentGlyph.right, y);
                }
            } else {
                const int step = (alignmentGlyph.right - alignmentGlyph.left) / 3;
                for (int i = 1; i <= 2; ++i) {
                    const int x = alignmentGlyph.left + i * step;
                    MoveToEx(hdc, x, alignmentGlyph.top, nullptr);
                    LineTo(hdc, x, alignmentGlyph.bottom);
                }
            }
        } else {
            // Half the glyph box, not a quarter -- confirmed,
            // human-reported, that a quarter-thickness bar read as too
            // subtle to register as "this is the tab strip's edge" at
            // this glyph's small size. Deliberate; don't shrink this
            // back for visual "balance" without re-confirming legibility
            // live.
            const int barThickness =
                std::max(1, static_cast<int>(alignmentGlyph.bottom - alignmentGlyph.top) / 2);
            HBRUSH barBrush = CreateSolidBrush(glyphColor);
            const RECT bar = (alignment_ == GroupAlignment::Vertical)
                                  ? RECT{alignmentGlyph.left, alignmentGlyph.top, alignmentGlyph.left + barThickness,
                                         alignmentGlyph.bottom}
                                  : RECT{alignmentGlyph.left, alignmentGlyph.top, alignmentGlyph.right,
                                         alignmentGlyph.top + barThickness};
            FillRect(hdc, &bar, barBrush);
            DeleteObject(barBrush);
        }
    }

    SelectObject(hdc, glyphPen);
    DeleteObject(actionGlyphPen);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(glyphPen);
}

void GroupChromeWindow::UpdateTooltip() {
    if (tooltipWindow_ == nullptr) {
        return;
    }
    std::wstring text;
    if (hoveredTitleBarButton_.has_value()) {
        switch (*hoveredTitleBarButton_) {
            case HTMINBUTTON:
                text = L"Minimize";
                break;
            case HTMAXBUTTON:
                text = IsZoomed(window_) ? L"Restore" : L"Maximize";
                break;
            case HTCLOSE:
                text = L"Close";
                break;
            default:
                break;
        }
    } else if (hoveredActionButton_.has_value()) {
        switch (*hoveredActionButton_) {
            case TitleBarActionButton::ModeToggle:
                // These two buttons name their *current* state, not what
                // clicking them would do -- matching their glyphs, which
                // already depict the current mode/alignment rather than
                // the target (see PaintTitleBar). An earlier "Switch to
                // X" wording named the target instead, which read as a
                // direct contradiction of the glyph sitting right under
                // the cursor. The remaining buttons below stay
                // action-worded because they *are* actions, with no
                // state of their own to report.
                switch (mode_) {
                    case GroupMode::Tab:
                        text = L"Tabs";
                        break;
                    case GroupMode::Tile:
                        text = L"Tiles";
                        break;
                    case GroupMode::Stack:
                        text = L"Stack";
                        break;
                }
                break;
            case TitleBarActionButton::ManageWindows:
                text = L"Edit Group Windows...";
                break;
            case TitleBarActionButton::TileMaximize:
                text = tileMaximized_ ? L"Restore tile" : L"Maximize tile";
                break;
            case TitleBarActionButton::Alignment:
                // One setting regardless of mode (see GroupAlignment's
                // own comment), so unlike the old target-naming wording
                // this needs no per-mode phrasing at all.
                text = alignment_ == GroupAlignment::Horizontal ? L"Horizontal" : L"Vertical";
                break;
        }
    }

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = window_;
    ti.uId = 1;

    if (text.empty()) {
        SendMessageW(tooltipWindow_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
        return;
    }
    // Applied fresh on every show, not just once at creation, so a
    // theme change without restarting Polish still takes effect (same
    // reasoning as ApplyDarkModeToMenu's own comment).
    ApplyDarkModeToTooltip(tooltipWindow_);
    ti.lpszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(tooltipWindow_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    POINT cursor{};
    GetCursorPos(&cursor);
    // A few px below-right of the cursor, not directly under it -- the
    // usual tooltip placement, so it doesn't sit on top of (and get
    // dismissed by) the very cursor that's hovering the button.
    SendMessageW(tooltipWindow_, TTM_TRACKPOSITION, 0, MAKELPARAM(cursor.x + 12, cursor.y + 20));
    SendMessageW(tooltipWindow_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

RECT GroupChromeWindow::ContentRectInScreenCoords() const {
    if (window_ == nullptr) {
        return RECT{};
    }
    RECT client;
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);

    POINT topLeft{client.left + TabStripLeftWidth(dpi), client.top + HeaderHeight(dpi)};
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
    return RECT{client.left + TabStripLeftWidth(dpi), client.top + HeaderHeight(dpi), client.right, client.bottom};
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
    // Vertical alignment's tab column extends below the header band
    // too (down the whole left edge, not just under the title bar) --
    // the rect above alone would leave it stale on a hover/active-tab
    // change.
    const int leftWidth = TabStripLeftWidth(dpi);
    if (leftWidth > 0) {
        RECT columnRect{client.left, client.top + HeaderHeight(dpi), client.left + leftWidth, client.bottom};
        InvalidateRect(window_, &columnRect, TRUE);
    }
}

void GroupChromeWindow::InvalidateTitleBar() {
    if (window_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    RECT titleBarRect{client.left, client.top, client.right, client.top + TitleBarHeight(dpi)};
    InvalidateRect(window_, &titleBarRect, TRUE);
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
    if (!IsTiledMode(mode_) || window_ == nullptr) {
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
    // size is actually needed (the outer rect and the client/content
    // rect are not the same rect).
    RECT windowRect{};
    GetWindowRect(window_, &windowRect);
    const int overheadWidth = (windowRect.right - windowRect.left) - currentWidth;
    const int overheadHeight = (windowRect.bottom - windowRect.top) - currentHeight;

    const int newWidth = std::max(static_cast<int>(minContentSize.cx), currentWidth) + overheadWidth;
    const int newHeight = std::max(static_cast<int>(minContentSize.cy), currentHeight) + overheadHeight;

    // SWP_NOMOVE -- top-left stays put, so this never generates a
    // WM_MOVE/onMoved_ notification; the caller re-applies layout
    // explicitly right after calling this, so there's no risk of a
    // feedback loop through that callback either way.
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

void GroupChromeWindow::SetTileMaximized(bool maximized) {
    tileMaximized_ = maximized;
    if (window_ != nullptr) {
        InvalidateTitleBar();
        UpdateTooltip();
    }
}

void GroupChromeWindow::SetAlignment(GroupAlignment alignment) {
    alignment_ = alignment;
    if (window_ != nullptr) {
        // Unlike SetMode/SetTileMaximized, this changes which axis the
        // whole header (title bar + tab strip) occupies -- a narrow
        // InvalidateTitleBar/InvalidateTabStrip wouldn't cover the
        // newly-reshaped content boundary either way (the left column
        // appearing/disappearing), so this is the one case that needs
        // the full-window invalidate SetMode itself already uses.
        InvalidateRect(window_, nullptr, TRUE);
        UpdateTooltip();
    }
}

void GroupChromeWindow::ShowContextMenu(int screenX, int screenY) {
    if (window_ == nullptr) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    // Three mutually-exclusive mode items (a radio group, not a single
    // cycling "Switch to X" item -- that stopped reading sensibly once
    // there were three modes to name instead of two) plus a separator
    // before the unrelated "Edit windows..." item.
    AppendMenuW(menu, MF_STRING, kContextMenuModeTab, L"Tabs");
    AppendMenuW(menu, MF_STRING, kContextMenuModeTile, L"Tiles");
    AppendMenuW(menu, MF_STRING, kContextMenuModeStack, L"Stack");
    CheckMenuRadioItem(menu, kContextMenuModeTab, kContextMenuModeStack,
                        mode_ == GroupMode::Tab   ? kContextMenuModeTab
                        : mode_ == GroupMode::Tile ? kContextMenuModeTile
                                                    : kContextMenuModeStack,
                        MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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

    if (cmd == kContextMenuModeTab && onModeSelected_) {
        onModeSelected_(GroupMode::Tab);
    } else if (cmd == kContextMenuModeTile && onModeSelected_) {
        onModeSelected_(GroupMode::Tile);
    } else if (cmd == kContextMenuModeStack && onModeSelected_) {
        onModeSelected_(GroupMode::Stack);
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
        // there (every mode only ever draws in the header/gap space
        // around members, never over them), so without this
        // style there was nothing stopping that from visually
        // interfering with the active member's own on-screen content.
        // This is the standard Win32 fix for a parent with child
        // windows it never means to paint over.
        window_ = CreateWindowExW(0, kWindowClassName, L"Group", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                   CW_USEDEFAULT, CW_USEDEFAULT, kDefaultChromeWidth, kDefaultChromeHeight, nullptr,
                                   nullptr, instance_, this);
        if (window_ != nullptr) {
            ApplyDarkTitleBar(window_, IsDarkModeEnabled());
            // kDefaultChromeWidth/Height above are logical (96 DPI) px,
            // but CreateWindowExW's own width/height parameters are
            // always raw physical pixels -- it has no way to know which
            // monitor (and therefore DPI) the window will actually land
            // on ahead of time, so it can't scale them itself. Rescale
            // now that GetDpiForWindow can report the real answer for
            // this now-real window -- confirmed real, human-reported: a
            // group created directly on a 200% monitor (no monitor
            // crossing to trigger WM_DPICHANGED's own equivalent fix)
            // kept the un-scaled physical size, making the whole window
            // effectively half its intended logical size on screen --
            // and this title bar band, correctly DPI-scaled on its own,
            // looked wildly oversized relative to that shrunken window.
            // SWP_FRAMECHANGED additionally forces Windows to re-run
            // WM_NCCALCSIZE and actually repaint the non-client/client
            // boundary right now, rather than leaving whatever it
            // composited during CreateWindowExW's own internal (pre-
            // WM_NCCALCSIZE-override) sizing pass on screen -- confirmed
            // real, human-reported: without this, the reclaimed former-
            // caption strip showed stale white until the window was
            // moved (which triggers its own frame recalculation as a
            // side effect) or something else forced a repaint.
            const UINT dpi = GetDpiForWindow(window_);
            const int width = Scale(kDefaultChromeWidth, dpi);
            const int height = Scale(kDefaultChromeHeight, dpi);
            SetWindowPos(window_, nullptr, 0, 0, width, height,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

            // One manually-tracked tool covering all five title-bar
            // buttons -- text and position are set fresh in
            // UpdateTooltip on every hover change, so a single tool
            // works instead of one per button. TTF_TRACK put it under
            // this code's own control (TTM_TRACKACTIVATE/
            // TTM_TRACKPOSITION) rather than the tooltip's own automatic
            // mouse-relay tracking, which only understands ordinary
            // client-area mouse messages, not the caption buttons'
            // WM_NCMOUSEMOVE.
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
