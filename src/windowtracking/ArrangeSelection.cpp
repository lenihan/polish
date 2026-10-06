#include "windowtracking/ArrangeSelection.h"

#include <algorithm>
#include <numeric>
#include <utility>

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

std::vector<HWND> ArrangeToggle::Next(ArrangeKind kind, const std::vector<HWND>& mruOrdered,
                                      const std::function<bool(const std::vector<HWND>&)>& fits,
                                      const std::function<long long(HWND)>& weightOf) {
    const int slots = SlotsFor(kind);
    if (static_cast<int>(mruOrdered.size()) < MinimumWindowsFor(kind)) {
        return {};
    }

    // The most recent `slots` windows. Anything older is left exactly
    // where it is: a tiling command is for the handful of windows actually
    // being worked in, and rearranging the rest of the desktop as a side
    // effect is how a convenience becomes something people stop using.
    const std::vector<HWND> chosen(mruOrdered.begin(), mruOrdered.begin() + slots);

    std::vector<HWND> key = chosen;
    std::sort(key.begin(), key.end());

    KindState& state = StateFor(kind);
    if (key != state.lastEligible) {
        // A different set of windows: the user means something new, so
        // start from MRU order again rather than inheriting a rotation
        // they asked for about some other set of windows.
        state.presses = 0;
        state.lastEligible = std::move(key);
    }

    // The arrangements this press can choose between, in the order
    // repeated presses step through them.
    //
    // One per rotation of the most-recent order: each window one slot
    // further along per press, the last wrapping to the first, so with N
    // slots every window leads once and passes through every slot. Built
    // from what was just chosen rather than a stored list, so it always
    // rotates the windows that are eligible now, even if their order
    // shifted since last time.
    //
    // A rotation that `fits` rejects is not dropped but *replaced* by the
    // closest arrangement that does fit -- the one with the fewest windows
    // in a different slot from that rotation. Dropping it was a bug: when
    // size limits tie two windows to a shared row, only some rotations fit,
    // so the ones left never visited every slot, and a window such as
    // Outlook never reached the upper right. Replacing keeps the cycle the
    // same length and keeps the window the rotation was about to put in a
    // given slot there wherever the limits allow it, moving the others
    // around it instead.
    std::vector<std::vector<HWND>> rotations;
    for (int k = 0; k < slots; ++k) {
        std::vector<HWND> rotated = chosen;
        std::rotate(rotated.begin(), rotated.end() - k, rotated.end());
        rotations.push_back(std::move(rotated));
    }

    std::vector<std::vector<HWND>> layouts;
    if (!fits) {
        layouts = rotations;
    } else {
        // Every arrangement of the windows that fits (at most 4! = 24).
        std::vector<std::vector<HWND>> fitting;
        std::vector<int> order(static_cast<size_t>(slots));
        for (int i = 0; i < slots; ++i) {
            order[static_cast<size_t>(i)] = i;
        }
        do {
            std::vector<HWND> arranged(static_cast<size_t>(slots));
            for (int slot = 0; slot < slots; ++slot) {
                arranged[static_cast<size_t>(slot)] = chosen[static_cast<size_t>(order[static_cast<size_t>(slot)])];
            }
            if (fits(arranged)) {
                fitting.push_back(std::move(arranged));
            }
        } while (std::next_permutation(order.begin(), order.end()));

        const auto differences = [](const std::vector<HWND>& a, const std::vector<HWND>& b) {
            int count = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                count += a[i] != b[i] ? 1 : 0;
            }
            return count;
        };
        const auto alreadyUsed = [&](const std::vector<HWND>& candidate) {
            return std::find(layouts.begin(), layouts.end(), candidate) != layouts.end();
        };
        // Rotations that fit keep their own place in the cycle, so claim
        // them first -- a substitute must not take one of their slots.
        std::vector<bool> rotationFits(rotations.size());
        for (size_t k = 0; k < rotations.size(); ++k) {
            rotationFits[k] = std::find(fitting.begin(), fitting.end(), rotations[k]) != fitting.end();
        }
        // (window, slot) pairs the lap already covers: every rotation that
        // fits, then each substitute as it is chosen.
        std::vector<std::pair<HWND, size_t>> visited;
        for (size_t k = 0; k < rotations.size(); ++k) {
            if (rotationFits[k]) {
                for (size_t slot = 0; slot < rotations[k].size(); ++slot) {
                    visited.emplace_back(rotations[k][slot], slot);
                }
            }
        }
        std::vector<std::vector<HWND>> reserved;
        for (size_t k = 0; k < rotations.size(); ++k) {
            if (rotationFits[k]) {
                reserved.push_back(rotations[k]);
            }
        }
        for (size_t k = 0; k < rotations.size(); ++k) {
            if (rotationFits[k]) {
                layouts.push_back(rotations[k]);
                continue;
            }
            const std::vector<HWND>* best = nullptr;
            int bestDistance = slots + 1;
            int bestNovelty = -1;
            long long bestCost = 0;
            for (const auto& candidate : fitting) {
                if (alreadyUsed(candidate) ||
                    std::find(reserved.begin(), reserved.end(), candidate) != reserved.end()) {
                    continue;
                }
                const int distance = differences(candidate, rotations[k]);
                // What it costs to move the windows this candidate moves:
                // the sum of their weights. Heavier means more stubborn --
                // a window that is hard to fit is the one that should stay
                // where the rotation put it, and the easy ones should move
                // around it. Without this a tie in distance could swap the
                // stubborn window itself, so Outlook jumped slots instead
                // of stepping A, B, C, D.
                long long cost = 0;
                if (weightOf) {
                    for (size_t slot = 0; slot < candidate.size(); ++slot) {
                        if (candidate[slot] != rotations[k][slot]) {
                            cost += weightOf(candidate[slot]);
                        }
                    }
                }
                // Failing that, prefer the arrangement that puts windows in
                // slots they have not been in so far this lap, so a tie is
                // not settled by whichever candidate came first.
                int novelty = 0;
                for (size_t slot = 0; slot < candidate.size(); ++slot) {
                    if (std::find(visited.begin(), visited.end(), std::make_pair(candidate[slot], slot)) ==
                        visited.end()) {
                        ++novelty;
                    }
                }
                const bool better = best == nullptr || cost < bestCost ||
                                    (cost == bestCost && distance < bestDistance) ||
                                    (cost == bestCost && distance == bestDistance && novelty > bestNovelty);
                if (better) {
                    bestCost = cost;
                    bestDistance = distance;
                    bestNovelty = novelty;
                    best = &candidate;
                }
            }
            if (best != nullptr) {
                layouts.push_back(*best);
                for (size_t slot = 0; slot < best->size(); ++slot) {
                    visited.emplace_back((*best)[slot], slot);
                }
            }
        }
    }
    if (layouts.empty()) {
        return {};
    }

    const std::vector<HWND> result = layouts[static_cast<size_t>(state.presses) % layouts.size()];
    // Kept bounded so a very long session cannot overflow it. The modulo
    // above keeps any value meaningful whatever the list's length is.
    state.presses = (state.presses + 1) % 720720;  // 720720 = lcm(1..16): stays in step for any list size
    return result;
}

}  // namespace polish
