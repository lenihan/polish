#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "windowtracking/StackState.h"
#include "windowtracking/StackStripLayout.h"

namespace polish {

// One tab to draw. The icon is borrowed (from the window or its class) and
// never destroyed here.
struct StripTab {
    std::wstring title;
    HICON icon = nullptr;
    // Drawn dimmed: the member is minimized, so clicking the tab will bring
    // it back rather than just raise it.
    bool minimized = false;
};

// The tab strip of a stack: a small always-on-top window that sits against
// the stack's rect (above it, or down its left side) and lets you raise a
// buried member.
//
// It is the *whole* user interface of a stack. There is no container window
// around the members -- they are ordinary top-level windows sharing a rect
// -- so this is the only thing Polish draws, and everything it does is
// asked for through the callbacks below; it holds no stack state of its own
// beyond what to paint.
//
// WS_EX_NOACTIVATE is essential, not tidy: clicking a tab must not take
// activation, because the click is about to raise and activate a *member*,
// and a strip that had stolen focus first would be the foreground window
// the member then has to wrest back. WS_EX_TOOLWINDOW keeps it off the
// taskbar and out of Alt+Tab. The same combination (topmost, no-activate,
// tool window) the tab hover popup already used.
//
// It has no position of its own. PlaceAt is told where to go on every
// reflow, derived from the stack rect (StackLayout.h); nothing here decides
// it, so the strip cannot drift away from its stack.
class StackStripWindow {
public:
    // The strip's thickness across the stack, in DIPs: a band along the top
    // (Horizontal) or a column down the left (Vertical).
    // A band along the top is a title bar above a row of tabs; a column down
    // the left carries the same title bar above its tab list.
    static constexpr int kTitleBarDip = 28;
    static constexpr int kTabRowDip = 36;
    static constexpr int kHorizontalThicknessDip = kTitleBarDip + kTabRowDip;
    static constexpr int kVerticalThicknessDip = 200;
    static int ThicknessPx(StackAlignment alignment, UINT dpi);

    explicit StackStripWindow(HINSTANCE instance);
    ~StackStripWindow();

    StackStripWindow(const StackStripWindow&) = delete;
    StackStripWindow& operator=(const StackStripWindow&) = delete;

    // The stack's name: shown in the title bar, and as the title of its
    // taskbar button.
    void SetTitle(const std::wstring& title);

    // The stack's presence in the taskbar. The strip itself cannot have a
    // taskbar button -- it is a topmost, never-activated tool window that is
    // hidden whenever no member is in front -- so a small separate window
    // stands in for the stack: a normal (app) window with the stack's name,
    // kept offscreen and one pixel in size, with an AppUserModelID of its own
    // so that each stack gets its own button instead of being grouped under
    // Polish's. The owner learns of the shell activating or closing it
    // through the callbacks below.
    HWND TaskbarHandle() const { return taskbarWindow_; }
    // Keeps the taskbar button in step with the stack: minimized exactly when
    // every member is. A button that looks "up" is one the shell has nothing
    // to restore, so clicking it did nothing.
    void SetTaskbarMinimized(bool minimized);

    // Replaces the tabs and which one is active, and repaints.
    void SetTabs(std::vector<StripTab> tabs, size_t activeIndex);
    void SetActiveIndex(size_t activeIndex);
    void SetAlignment(StackAlignment alignment);

    // Puts the strip exactly at `screenRect` (device pixels), above
    // everything non-topmost, without activating it.
    void PlaceAt(const RECT& screenRect);
    void Show();
    void Hide();
    bool IsShown() const;

    HWND Handle() const { return window_; }
    RECT ScreenRect() const;
    bool ContainsScreenPoint(POINT screenPt) const;

    // Drag-to-join feedback: an insertion caret before tab `index` (or after
    // the last, for tabs.size()), or none. A window being dragged over the
    // strip sets this as it moves.
    void SetDropCaret(std::optional<size_t> insertionIndex);
    // Where a window dropped at `screenPt` would be inserted.
    size_t InsertionIndexAtScreenPoint(POINT screenPt) const;

    // A tab was clicked (pressed and released without being dragged).
    void SetOnTabClicked(std::function<void(size_t)> callback) { onTabClicked_ = std::move(callback); }
    // A dragged tab crossed into another's slot: (from, to). Fires live,
    // once per crossing. The owner reorders its state and calls SetTabs.
    void SetOnTabReordered(std::function<void(size_t, size_t)> callback) { onTabReordered_ = std::move(callback); }
    // A dragged tab was released well clear of the strip: take that window
    // out of the stack and put it down at this screen point.
    void SetOnTabTornOut(std::function<void(size_t, POINT)> callback) { onTabTornOut_ = std::move(callback); }
    // The strip's own empty area was dragged: move the whole stack by this
    // much since the last call.
    void SetOnStripDragged(std::function<void(int, int)> callback) { onStripDragged_ = std::move(callback); }
    // The title bar's minimize button: minimize every member.
    void SetOnMinimizeAll(std::function<void()> callback) { onMinimizeAll_ = std::move(callback); }
    // The title bar was double-clicked: the owner opens the rename/edit UI.
    void SetOnTitleDoubleClicked(std::function<void()> callback) { onTitleDoubleClicked_ = std::move(callback); }
    // The taskbar button's "Close window": the stack is being closed. The
    // members are left exactly where they are.
    // The shell sent the stand-in a minimize or restore command, which happens
    // when its button is clicked while the stand-in is already the foreground
    // window (it stays there after the stack is minimized). The owner toggles
    // the stack.
    void SetOnTaskbarToggle(std::function<void()> callback) { onTaskbarToggle_ = std::move(callback); }
    void SetOnTaskbarCloseRequested(std::function<void()> callback) {
        onTaskbarCloseRequested_ = std::move(callback);
    }
    // Right click, over a tab or not.
    void SetOnContextMenu(std::function<void(POINT, std::optional<size_t>)> callback) {
        onContextMenu_ = std::move(callback);
    }

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TaskbarProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, const RECT& client);
    // The title bar band, and the part of the client area below it that holds
    // the tabs (every tab rect and hit test is in this region).
    RECT TitleBarRect(const RECT& client, UINT dpi) const;
    RECT TabAreaRect(const RECT& client, UINT dpi) const;
    RECT MinimizeButtonRect(const RECT& client, UINT dpi) const;
    TabMetrics MetricsFor(UINT dpi) const;
    std::vector<RECT> CurrentTabRects() const;
    void EndDrag();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND taskbarWindow_ = nullptr;
    std::wstring title_;
    bool minimizeHovered_ = false;
    bool minimizePressed_ = false;
    std::vector<StripTab> tabs_;
    size_t activeIndex_ = 0;
    StackAlignment alignment_ = StackAlignment::Horizontal;
    std::optional<size_t> dropCaret_;
    std::optional<size_t> hoveredTab_;
    bool trackingMouseLeave_ = false;

    // A press on a tab: becomes a click if released in place, a reorder if
    // dragged along the strip, a tear-out if dragged clear of it.
    std::optional<size_t> pressedTab_;
    POINT pressScreenPt_{};
    bool tabDragging_ = false;
    bool tearing_ = false;
    // A press on the strip's empty area: moves the whole stack.
    bool stripDragging_ = false;
    POINT lastScreenPt_{};

    std::function<void(size_t)> onTabClicked_;
    std::function<void(size_t, size_t)> onTabReordered_;
    std::function<void(size_t, POINT)> onTabTornOut_;
    std::function<void(int, int)> onStripDragged_;
    std::function<void(POINT, std::optional<size_t>)> onContextMenu_;
    std::function<void()> onMinimizeAll_;
    std::function<void()> onTitleDoubleClicked_;
    std::function<void()> onTaskbarCloseRequested_;
    std::function<void()> onTaskbarToggle_;
};

}  // namespace polish
