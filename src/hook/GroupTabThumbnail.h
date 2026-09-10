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
// Not a PrintWindow(..., PW_RENDERFULLCONTENT) capture taken live at
// hover time either -- also tried, and also confirmed blank for some
// apps (Settings, Outlook) via a compiled spike: every non-active tab's
// member is WS_HIDE'n by the time a hover can even happen, and
// PrintWindow only reliably returns real content for a *visible*
// window -- composited apps (Settings/Outlook) return a blank capture
// once hidden, even though GDI-classic apps (Notepad) happen to still
// work. So capturing has to happen *before* a member is hidden, not
// lazily on hover -- see GroupManager::CachedThumbnail, which captures
// at the exact moment a member transitions from visible to hidden and
// hands the result here. This class just displays whatever bitmap it's
// given; it no longer does any capturing of its own.
class GroupTabThumbnail {
public:
    explicit GroupTabThumbnail(HINSTANCE instance);
    ~GroupTabThumbnail();

    GroupTabThumbnail(const GroupTabThumbnail&) = delete;
    GroupTabThumbnail& operator=(const GroupTabThumbnail&) = delete;

    // Shows a copy of `snapshot` (borrowed -- not taken ownership of;
    // copied internally so it's safe even if the caller's own copy is
    // later replaced/freed), positioned just below `tabScreenRect`
    // (screen coordinates -- the hovered tab's own rect), or to its
    // right when `preferRightSide` is true (the caller's own
    // Vertical-alignment tab strip runs down the left edge, so
    // "below" would risk overlapping the next tab row down -- the
    // tab rect's own shape can't be used to infer this: a vertical
    // tab row is a fixed-width column that's wide and short, not
    // narrow and tall, so it looks "horizontal" by shape alone). A
    // null `snapshot` (e.g. no capture exists yet for this member)
    // leaves whatever was previously shown in place, or a plain
    // placeholder background if nothing has ever been shown. Safe to
    // call repeatedly for a different member while already showing
    // one.
    void ShowFor(HBITMAP snapshot, const RECT& tabScreenRect, bool preferRightSide);

    // Hides the preview. Safe to call when already hidden.
    void Hide();

private:
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HBITMAP snapshot_ = nullptr;  // this class's own copy
};

}  // namespace polish
