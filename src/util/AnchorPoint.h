#pragma once

#include <windows.h>

namespace polish {

// Where a resolved anchor point came from -- logged once per animation so
// a live walkthrough can tell which apps actually expose a real caret.
enum class AnchorSource { Caret, Cursor, WindowCenter, None };

struct Anchor {
    POINT point{};
    AnchorSource source = AnchorSource::None;
};

// Best guess at "where the user is looking" for a copy/paste that just
// happened, in screen coordinates. In order:
//   1. The foreground thread's text caret (GetGUIThreadInfo), if it has a
//      real Win32 caret and that caret is inside the foreground window.
//      Many apps (Chromium, Electron, UWP) draw their own caret and
//      expose none, so this often misses.
//   2. The mouse cursor, if it's inside the foreground window -- a cursor
//      parked over some other window says nothing about this copy.
//   3. The centre of the foreground window's visible rect.
// AnchorSource::None (point unset) if there's no usable foreground window.
Anchor ResolveInteractionAnchor();

}  // namespace polish
