#include "windowtracking/StickyEdges.h"

#include <algorithm>
#include <cstdlib>

namespace polish {

namespace {

bool Inside(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

POINT ClampInto(const RECT& r, POINT p) {
    return POINT{std::clamp(p.x, r.left, r.right - 1), std::clamp(p.y, r.top, r.bottom - 1)};
}

// How far `p` is inside `r` from its nearest edge (0 on the edge row/column).
int DepthInside(const RECT& r, POINT p) {
    return std::min({p.x - r.left, r.right - 1 - p.x, p.y - r.top, r.bottom - 1 - p.y});
}

int Overshoot(POINT to, POINT clamped) { return std::abs(to.x - clamped.x) + std::abs(to.y - clamped.y); }

}  // namespace

StickyResult StepSticky(std::span<const Barrier> barriers, POINT from, POINT to, int threshold, StickyState& state,
                        ULONGLONG now) {
    // Released barriers in this same move: their clamp point, so a barrier
    // sharing the edge is let go with them.
    POINT releasedAt{};
    bool released = false;

    for (size_t i = 0; i < barriers.size(); ++i) {
        const RECT& rect = barriers[i].rect;
        if (!Inside(rect, from) || Inside(rect, to)) {
            continue;
        }
        const POINT clamped = ClampInto(rect, to);
        if (released && std::abs(clamped.x - releasedAt.x) <= kStickySameEdgePx &&
            std::abs(clamped.y - releasedAt.y) <= kStickySameEdgePx) {
            continue;  // the same edge as the one just pushed through
        }
        const int index = static_cast<int>(i);
        if (state.barrier != index || now - state.lastPushTick > kStickyPushResetMs) {
            state = StickyState{index, 0, now};
        }
        state.pushed += Overshoot(to, clamped);
        state.lastPushTick = now;
        if (state.pushed >= threshold) {
            state = StickyState{};
            released = true;
            releasedAt = clamped;
            continue;  // an outer barrier may still hold it
        }
        return StickyResult{true, clamped};
    }

    // Not held. Backing away from the edge being pushed starts over.
    if (state.barrier >= 0 && static_cast<size_t>(state.barrier) < barriers.size()) {
        const RECT& rect = barriers[static_cast<size_t>(state.barrier)].rect;
        if (!Inside(rect, to) || DepthInside(rect, to) > kStickyBackOffPx) {
            state = StickyState{};
        }
    } else {
        state = StickyState{};
    }
    return StickyResult{false, to};
}

}  // namespace polish
