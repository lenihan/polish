#pragma once

#include <windows.h>

#include <dwmapi.h>

namespace polish {

// A small popup that shows a live DWM thumbnail of one group member,
// for hovering over a tab to preview it before switching -- "confirmed
// via M0(b) of the original Alt+Tab plan: DwmRegisterThumbnail on a
// plain, non-layered WS_POPUP window works (returns S_OK); no
// WS_EX_LAYERED needed, DWM composites the live preview directly.
class GroupTabThumbnail {
public:
    explicit GroupTabThumbnail(HINSTANCE instance);
    ~GroupTabThumbnail();

    GroupTabThumbnail(const GroupTabThumbnail&) = delete;
    GroupTabThumbnail& operator=(const GroupTabThumbnail&) = delete;

    // Shows a live preview of `member`'s current content, positioned
    // just below `tabScreenRect` (screen coordinates -- the hovered
    // tab's own rect). Safe to call repeatedly for a different member
    // while already showing one (re-registers).
    void ShowFor(HWND member, const RECT& tabScreenRect);

    // Hides the preview and unregisters the DWM thumbnail. Safe to call
    // when already hidden.
    void Hide();

private:
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
};

}  // namespace polish
