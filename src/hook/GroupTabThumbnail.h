#pragma once

#include <windows.h>

namespace polish {

// A small popup that shows a snapshot preview of one group member, for
// hovering over a tab to see which window it is before switching.
//
// Not a live DWM thumbnail (DwmRegisterThumbnail) -- tried that first,
// and it came back blank: DWM thumbnails work from a *top-level*
// window's own composited surface, but a group member is a WS_CHILD
// window now (see the reparenting rework) with no independent surface
// of its own to thumbnail -- DWM composites the whole chrome (parent +
// every child) as one surface, so asking for a thumbnail of just a
// child portion of that isn't something the API is built for. Confirmed
// by a real, blank-preview report, not assumed.
//
// PrintWindow(..., PW_RENDERFULLCONTENT) instead: a static snapshot
// captured at hover time (not live-updating), but it works at the
// window-content level rather than relying on DWM's compositing
// architecture, so it correctly captures a child window's actual
// content -- including hardware-accelerated/DirectComposition-rendered
// content, which is exactly what PW_RENDERFULLCONTENT was added for.
class GroupTabThumbnail {
public:
    explicit GroupTabThumbnail(HINSTANCE instance);
    ~GroupTabThumbnail();

    GroupTabThumbnail(const GroupTabThumbnail&) = delete;
    GroupTabThumbnail& operator=(const GroupTabThumbnail&) = delete;

    // Captures a fresh snapshot of `member`'s current content and shows
    // it, positioned just below `tabScreenRect` (screen coordinates --
    // the hovered tab's own rect). Safe to call repeatedly for a
    // different member while already showing one.
    void ShowFor(HWND member, const RECT& tabScreenRect);

    // Hides the preview. Safe to call when already hidden.
    void Hide();

private:
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HBITMAP snapshot_ = nullptr;
};

}  // namespace polish
