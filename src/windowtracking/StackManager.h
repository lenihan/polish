#pragma once

#include <windows.h>

#include <vector>

#include "windowtracking/StackState.h"

namespace polish {

// Owns every stack Polish currently knows about, and puts their members
// where they belong.
//
// A stack is a rectangle (StackState::Rect) and N ordinary top-level windows
// that all occupy the content part of it, with a tab strip beside them
// (StackLayout.h, hook/StackStripWindow.h). The manager does not own the
// strip and has no part in drawing; it is state plus the one thing that
// touches other processes' windows, which is moving them.
//
// Members are real top-level windows, never reparented. An earlier design
// made them children of a container window, which many windows refuse
// outright (a UWP frame fails SetParent every time), and which needed a
// resize-band overlay and a screenshot pipeline to hide the consequences of
// being a child. None of that exists any more.
class StackManager {
public:
    // Creates a stack containing exactly `windows`, in the given order, and
    // returns its id. An empty list is allowed. Places nothing: that happens
    // when the caller first reflows it.
    StackId CreateStack(const std::vector<HWND>& windows);

    // Forgets the stack. Does not touch its members: closing a stack leaves
    // every window exactly where it is.
    void RemoveStack(StackId id);

    size_t StackCount() const { return stacks_.size(); }
    const std::vector<StackState>& Stacks() const { return stacks_; }

    // nullptr if no stack has this id.
    StackState* FindStack(StackId id);
    const StackState* FindStack(StackId id) const;

    // nullptr if hwnd is not a member of any stack.
    StackState* FindStackContaining(HWND hwnd);

    // The smallest content size every non-minimized member will accept, i.e.
    // the largest minimum width and the largest minimum height among them
    // (each in visible-rect space, see WindowPlacement.h). Zero in either
    // dimension when no member imposes one.
    //
    // Windows refuse to go below their own minimum, so a content area
    // smaller than this would have a member overrunning it. Callers grow the
    // stack to at least this before placing anything.
    SIZE MinimumContentSize(const StackState& stack) const;

    // Moves every non-minimized member so its visible rect is `content`, and
    // brings the active one to the front. Members share one rect, so the
    // active member is what you see and the others are behind it.
    //
    // A minimized member is skipped, not restored: it is out of the way on
    // purpose, and clicking its tab is how it comes back.
    void PlaceMembers(const StackState& stack, const RECT& content) const;

    // Raises the active member above the others without moving anything.
    void RaiseActive(const StackState& stack) const;

private:
    std::vector<StackState> stacks_;
    StackId nextId_ = 1;
};

}  // namespace polish
