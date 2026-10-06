#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "windowtracking/WindowLayout.h"

namespace polish {

// Which windows a tiling command acts on, and in what order -- the policy
// layer over WindowLayout.h's geometry.
//
// Separate from main.cpp, and pure, for a reason that is structural rather
// than stylistic: polish_tests links polish_core only, so anything living
// in main.cpp cannot be unit tested at all. The rotate-on-repeat toggle below
// has two failure modes that are each one line of code and invisible in
// use until they annoy someone, which is exactly the kind of thing that
// has to be a test rather than a careful read.

// The fewest windows this kind can do anything with. Equal to
// SlotsFor(kind): "Tile 3-way" with two windows would have to either leave
// a third of the screen empty or quietly become 2-way, and both are worse
// than the command being unavailable and saying so.
int MinimumWindowsFor(ArrangeKind kind);

struct ArrangeAvailability {
    bool enabled;
    // Why not, when enabled is false -- short enough to go in the menu
    // item's own text. Win32 menus cannot carry a per-item tooltip (see
    // util/DarkMode.cpp on why this app has no owner-drawn menus), so the
    // text is the only place the reason can go. Empty when enabled.
    const wchar_t* reason;
};

ArrangeAvailability EvaluateArrange(ArrangeKind kind, int eligibleCount);

// Picks the windows for one invocation of a tiling command, and remembers
// enough between invocations to rotate the windows one slot on a repeat.
//
// The rotation exists because the first guess at which window should get
// the first slot is wrong most of the time, and pressing the same key
// again is a cheaper fix than dragging. Each repeat moves every window one
// slot along, the last wrapping round to the first, so with N slots every
// window is first exactly once in N presses and the Nth repeat returns to
// where it started. For two slots that is simply a swap.
class ArrangeToggle {
public:
    // mruOrdered is every eligible window, most recently used first.
    // Returns the ones to place, in placement order -- at most
    // SlotsFor(kind) of them, and empty if there are not enough.
    //
    // fits, if given, says whether an arrangement (the windows in slot
    // order) is acceptable -- main.cpp passes one that checks minimum
    // window sizes. Arrangements it rejects are never returned; see the
    // implementation for how the choice is made when it rejects some or
    // all of the plain rotations. Returns empty if none is acceptable.
    //
    // weightOf, if given, says how stubborn a window is (main.cpp passes
    // its minimum size). When a rotation does not fit and another
    // arrangement is substituted, it is the one that moves the lightest
    // windows, so the stubborn ones keep the slot the rotation gave them.
    std::vector<HWND> Next(ArrangeKind kind, const std::vector<HWND>& mruOrdered,
                           const std::function<bool(const std::vector<HWND>&)>& fits = {},
                           const std::function<long long(HWND)>& weightOf = {});

    // Forget the rotation state for every kind, so the next invocation is
    // in MRU order.
    void Reset();

private:
    struct KindState {
        // The eligible windows last seen, sorted. Sorted, not in MRU
        // order, and this is the subtle part: the point of the key is to
        // notice "a different set of windows, so the user means something
        // new", and MRU order changes every time anything is focused --
        // which is nearly always, between two presses of the same key.
        // Keying on the order would therefore reset the toggle almost
        // every time and the rotation would look broken.
        //
        // Nor can it be the *chosen* order: that changes as a direct
        // result of rotating, so the toggle would see its own effect as a
        // change, reset itself, and never rotate twice in a row.
        std::vector<HWND> lastEligible;
        // How many times this kind has been invoked for the current set of
        // windows; selects which arrangement the next press shows.
        int presses = 0;
    };

    KindState& StateFor(ArrangeKind kind);

    // One per kind, not shared. 2-way and 3-way are different commands and
    // using one must not leave another halfway through its own toggle.
    KindState twoWay_;
    KindState threeWay_;
    KindState fourWay_;
};

}  // namespace polish
