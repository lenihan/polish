#pragma once

#include <windows.h>

namespace polish {

// Where a resolved anchor point came from -- logged once per animation so
// a live walkthrough can tell which apps actually expose a real caret.
enum class AnchorSource { Selection, Caret, Cursor, WindowCenter, None };

struct Anchor {
    POINT point{};
    AnchorSource source = AnchorSource::None;
};

// Best guess at "where the user is looking" for a copy/paste that just
// happened, in screen coordinates.
//
// The centre of the text selection beats all of these when one is
// available, but it can only be had through UI Automation, which is too
// slow to resolve here -- main.cpp asks UiaWorker for it off-thread and
// uses this as the fallback chain. In order:
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
