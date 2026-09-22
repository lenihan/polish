#include "hook/TaskbarShield.h"

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishTaskbarShield";

// Out of 255, and it must not be 0 -- see the class comment. This is the
// smallest value that still leaves the window in the hit-test.
constexpr BYTE kShieldAlpha = 1;

LRESULT CALLBACK ShieldProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) {
        // Claims the point, which is the whole feature: the taskbar never
        // sees the pointer here, so its flyout dwell never starts.
        //
        // Unconditional on purpose. This was once conditional -- answer
        // HTTRANSPARENT while a button was down, so clicks would fall
        // through to the taskbar -- and that does not work across a
        // process boundary at all (see SetPassThrough, which is what
        // replaced it). Nothing about the hit-test can distinguish a
        // hover from a click anyway: there is one verdict per dispatch
        // and it carries no idea what it is being asked for.
        //
        // DefWindowProcW would answer HTCLIENT for this window regardless
        // (a WS_POPUP with no frame is all client area). Spelled out so
        // the answer is a decision rather than a default.
        return HTCLIENT;
    }
    if (msg == WM_ERASEBKGND) {
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ShieldProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    // No cursor of its own: a shield that set one would change the
    // pointer's appearance over the taskbar, which is exactly the kind of
    // visible tell this window exists to avoid. With none, the cursor
    // keeps whatever it already was.
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

TaskbarShield::TaskbarShield(HINSTANCE instance) : instance_(instance) { EnsureClassRegistered(instance); }

TaskbarShield::~TaskbarShield() {
    for (const Shield& shield : shields_) {
        if (shield.window != nullptr) {
            DestroyWindow(shield.window);
        }
    }
}

HWND TaskbarShield::CreateShieldWindow() {
    // WS_EX_TOPMOST so it sits above Shell_TrayWnd (measured at z-depth 8
    // against the taskbar's 14 -- a plain topmost window is enough, which
    // is the same thing the bullseye already relies on).
    // WS_EX_NOACTIVATE so clicking near it never steals foreground.
    // WS_EX_TOOLWINDOW to keep it out of the native Alt+Tab list.
    // WS_EX_LAYERED for the alpha.
    //
    // Created absorbing -- WS_EX_TRANSPARENT is added and removed later,
    // per gesture, by SetPassThrough. A shield born with it would let the
    // very first hover through and show the flyout once.
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName,
                                  L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, nullptr);
    if (window != nullptr) {
        SetLayeredWindowAttributes(window, 0, kShieldAlpha, LWA_ALPHA);
        if (passThrough_) {
            // A taskbar that appeared mid-gesture (explorer restarting
            // under a held button) must match the shields that already
            // exist, or it would absorb an event its siblings pass on.
            SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        }
    }
    return window;
}

void TaskbarShield::SetPassThrough(bool passThrough) {
    if (passThrough == passThrough_) {
        return;
    }
    passThrough_ = passThrough;
    for (const Shield& shield : shields_) {
        if (shield.window == nullptr) {
            continue;
        }
        const LONG_PTR current = GetWindowLongPtrW(shield.window, GWL_EXSTYLE);
        const LONG_PTR updated = passThrough ? (current | WS_EX_TRANSPARENT) : (current & ~WS_EX_TRANSPARENT);
        // No SetWindowPos/SWP_FRAMECHANGED afterwards: WS_EX_TRANSPARENT
        // changes hit-testing, not the frame, and this runs from inside
        // the low-level mouse hook where anything that could pump
        // messages is out of the question.
        SetWindowLongPtrW(shield.window, GWL_EXSTYLE, updated);
    }
}

void TaskbarShield::Update(const std::vector<TaskbarButton>& buttons) {
    // The union of each taskbar's own buttons. Taskbars are keyed by
    // HMONITOR rather than by index because AutomationId values repeat
    // across taskbars -- the same app pinned on two monitors gives two
    // buttons that are identical but for this.
    struct Strip {
        HMONITOR taskbar;
        RECT rect;
    };
    std::vector<Strip> strips;
    for (const TaskbarButton& button : buttons) {
        auto existing = std::find_if(strips.begin(), strips.end(),
                                     [&button](const Strip& strip) { return strip.taskbar == button.taskbar; });
        if (existing == strips.end()) {
            strips.push_back(Strip{button.taskbar, button.rect});
            continue;
        }
        UnionRect(&existing->rect, &existing->rect, &button.rect);
    }

    for (const Strip& strip : strips) {
        auto shield = std::find_if(shields_.begin(), shields_.end(),
                                   [&strip](const Shield& candidate) { return candidate.taskbar == strip.taskbar; });
        if (shield == shields_.end()) {
            shields_.push_back(Shield{strip.taskbar, CreateShieldWindow(), RECT{}});
            shield = shields_.end() - 1;
        }
        if (shield->window == nullptr) {
            continue;
        }
        // Re-asserting HWND_TOPMOST on every update, not only on a move:
        // another topmost window created since the last one would
        // otherwise sit above this and take the hover back. The rect
        // check only skips the move, never the z-order.
        const bool moved = !EqualRect(&shield->rect, &strip.rect);
        SetWindowPos(shield->window, HWND_TOPMOST, strip.rect.left, strip.rect.top, strip.rect.right - strip.rect.left,
                     strip.rect.bottom - strip.rect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | (moved ? 0 : SWP_NOMOVE | SWP_NOSIZE));
        shield->rect = strip.rect;
    }

    // A taskbar that has gone away (monitor disconnected) or has no app
    // buttons left on it. Hidden rather than destroyed -- monitors come
    // back, and an empty taskbar fills up again the moment an app opens.
    for (Shield& shield : shields_) {
        const bool stillCovered = std::any_of(strips.begin(), strips.end(), [&shield](const Strip& strip) {
            return strip.taskbar == shield.taskbar;
        });
        if (!stillCovered && shield.window != nullptr) {
            ShowWindow(shield.window, SW_HIDE);
            shield.rect = RECT{};
        }
    }
}

void TaskbarShield::Hide() {
    for (Shield& shield : shields_) {
        if (shield.window != nullptr) {
            ShowWindow(shield.window, SW_HIDE);
            shield.rect = RECT{};
        }
    }
}

}  // namespace polish
