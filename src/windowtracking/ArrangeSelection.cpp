#include "windowtracking/ArrangeSelection.h"

#include <algorithm>

namespace polish {

int MinimumWindowsFor(ArrangeKind kind) {
    return SlotsFor(kind);
}

ArrangeAvailability EvaluateArrange(ArrangeKind kind, int eligibleCount) {
    if (eligibleCount >= MinimumWindowsFor(kind)) {
        return ArrangeAvailability{true, L""};
    }
    // Spelled out per kind rather than built from the number, because
    // these go in front of the user and "needs 2 windows" reads better
    // than a sentence assembled from parts. Singular/plural matters at 2.
    switch (kind) {
        case ArrangeKind::TwoWay:
            return ArrangeAvailability{false, L"needs 2 windows"};
        case ArrangeKind::ThreeWay:
            return ArrangeAvailability{false, L"needs 3 windows"};
        case ArrangeKind::FourWay:
            return ArrangeAvailability{false, L"needs 4 windows"};
    }
    return ArrangeAvailability{false, L"not available"};
}

ArrangeToggle::KindState& ArrangeToggle::StateFor(ArrangeKind kind) {
    switch (kind) {
        case ArrangeKind::TwoWay:
            return twoWay_;
        case ArrangeKind::ThreeWay:
            return threeWay_;
        case ArrangeKind::FourWay:
            break;
    }
    return fourWay_;
}

void ArrangeToggle::Reset() {
    twoWay_ = KindState{};
    threeWay_ = KindState{};
    fourWay_ = KindState{};
}

std::vector<HWND> ArrangeToggle::Next(ArrangeKind kind, const std::vector<HWND>& mruOrdered) {
    const int slots = SlotsFor(kind);
    if (static_cast<int>(mruOrdered.size()) < MinimumWindowsFor(kind)) {
        return {};
    }

    // The most recent `slots` windows. Anything older is left exactly
    // where it is: a tiling command is for the handful of windows actually
    // being worked in, and rearranging the rest of the desktop as a side
    // effect is how a convenience becomes something people stop using.
    std::vector<HWND> chosen(mruOrdered.begin(), mruOrdered.begin() + slots);

    std::vector<HWND> key = chosen;
    std::sort(key.begin(), key.end());

    KindState& state = StateFor(kind);
    if (key != state.lastEligible) {
        // A different set of windows: the user means something new, so
        // start from MRU order again rather than inheriting a rotation
        // they asked for about some other set of windows.
        state.shiftNext = 0;
        state.lastEligible = std::move(key);
    }

    if (state.shiftNext > 0) {
        // Every window moves one slot further along per repeat, the last
        // wrapping to the first: [a,b,c] -> [c,a,b] -> [b,c,a] -> [a,b,c].
        // Applied to what was just chosen rather than to a stored list, so
        // it always rotates the windows that are eligible now, even if
        // their order shifted since last time.
        std::rotate(chosen.begin(), chosen.end() - state.shiftNext, chosen.end());
    }
    state.shiftNext = (state.shiftNext + 1) % slots;
    return chosen;
}

}  // namespace polish
