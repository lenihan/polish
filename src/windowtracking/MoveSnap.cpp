#include "windowtracking/MoveSnap.h"

#include <algorithm>
#include <cstdlib>

namespace polish {
namespace {

void SortUnique(std::vector<LONG>& edges) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
}

// The delta that would put `value` on the nearest candidate, or 0 if none
// is within `threshold`.
//
// "Nothing snapped" and "snapped, but it was already flush" both come
// back as 0, which is fine everywhere this is used: both mean don't move.
LONG BestDelta(const std::vector<LONG>& candidates, LONG value, int threshold) {
    LONG best = 0;
    LONG bestDistance = static_cast<LONG>(threshold) + 1;
    for (const LONG candidate : candidates) {
        const LONG delta = candidate - value;
        const LONG distance = std::abs(delta);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = delta;
        }
    }
    return bestDistance <= threshold ? best : 0;
}

// One translation delta for a whole axis of a Grip::Move drag: both edges
// are offered to the candidate set and the closer win decides, so a
// window can be snapped by whichever of its two edges the user brought
// near something.
LONG MoveDelta(const std::vector<LONG>& candidates, LONG nearEdge, LONG farEdge, int threshold) {
    const LONG nearDelta = BestDelta(candidates, nearEdge, threshold);
    const LONG farDelta = BestDelta(candidates, farEdge, threshold);
    if (nearDelta == 0) {
        return farDelta;
    }
    if (farDelta == 0) {
        return nearDelta;
    }
    return std::abs(farDelta) < std::abs(nearDelta) ? farDelta : nearDelta;
}

// Translation delta that pulls an axis back inside [nearLimit, farLimit].
// The near edge is applied second so it wins for a window too large to
// fit -- see ClampToMonitor's header comment.
LONG MoveClampDelta(LONG nearEdge, LONG farEdge, LONG nearLimit, LONG farLimit) {
    LONG delta = 0;
    if (farEdge > farLimit) {
        delta = farLimit - farEdge;
    }
    if (nearEdge + delta < nearLimit) {
        delta = nearLimit - nearEdge;
    }
    return delta;
}

// Nearest candidate strictly beyond `edge` in `direction`, or `edge`
// itself when there is none (meaning: don't move).
LONG LeadingTarget(const std::vector<LONG>& sorted, LONG edge, int direction) {
    if (direction > 0) {
        const auto it = std::upper_bound(sorted.begin(), sorted.end(), edge);
        return it == sorted.end() ? edge : *it;
    }
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), edge);
    return it == sorted.begin() ? edge : *(it - 1);
}

bool GripMovesLeftEdge(Grip grip) {
    return grip == Grip::TopLeft || grip == Grip::BottomLeft;
}

bool GripMovesTopEdge(Grip grip) {
    return grip == Grip::TopLeft || grip == Grip::TopRight;
}

}  // namespace

Grip GripForPoint(const RECT& visible, POINT pt) {
    const LONG centerX = visible.left + (visible.right - visible.left) / 2;
    const LONG centerY = visible.top + (visible.bottom - visible.top) / 2;
    const bool left = pt.x < centerX;
    const bool top = pt.y < centerY;
    if (top) {
        return left ? Grip::TopLeft : Grip::TopRight;
    }
    return left ? Grip::BottomLeft : Grip::BottomRight;
}

RECT DragGrip(const RECT& start, Grip grip, int dx, int dy) {
    RECT result = start;
    if (grip == Grip::Move) {
        result.left += dx;
        result.right += dx;
        result.top += dy;
        result.bottom += dy;
        return result;
    }
    if (GripMovesLeftEdge(grip)) {
        result.left += dx;
    } else {
        result.right += dx;
    }
    if (GripMovesTopEdge(grip)) {
        result.top += dy;
    } else {
        result.bottom += dy;
    }
    return result;
}

SnapCandidates CollectSnapEdges(const RECT& workArea, std::span<const RECT> others) {
    SnapCandidates candidates;
    candidates.xEdges.reserve(2 + others.size() * 2);
    candidates.yEdges.reserve(2 + others.size() * 2);
    candidates.xEdges.push_back(workArea.left);
    candidates.xEdges.push_back(workArea.right);
    candidates.yEdges.push_back(workArea.top);
    candidates.yEdges.push_back(workArea.bottom);
    for (const RECT& other : others) {
        candidates.xEdges.push_back(other.left);
        candidates.xEdges.push_back(other.right);
        candidates.yEdges.push_back(other.top);
        candidates.yEdges.push_back(other.bottom);
    }
    SortUnique(candidates.xEdges);
    SortUnique(candidates.yEdges);
    return candidates;
}

RECT ApplySnap(const RECT& desired, const SnapCandidates& candidates, int thresholdPx, Grip grip) {
    RECT result = desired;
    if (grip == Grip::Move) {
        const LONG dx = MoveDelta(candidates.xEdges, desired.left, desired.right, thresholdPx);
        const LONG dy = MoveDelta(candidates.yEdges, desired.top, desired.bottom, thresholdPx);
        result.left += dx;
        result.right += dx;
        result.top += dy;
        result.bottom += dy;
        return result;
    }
    if (GripMovesLeftEdge(grip)) {
        result.left += BestDelta(candidates.xEdges, desired.left, thresholdPx);
    } else {
        result.right += BestDelta(candidates.xEdges, desired.right, thresholdPx);
    }
    if (GripMovesTopEdge(grip)) {
        result.top += BestDelta(candidates.yEdges, desired.top, thresholdPx);
    } else {
        result.bottom += BestDelta(candidates.yEdges, desired.bottom, thresholdPx);
    }
    return result;
}

RECT ClampToMonitor(const RECT& desired, const RECT& monitor, Grip grip) {
    RECT result = desired;
    if (grip == Grip::Move) {
        const LONG dx = MoveClampDelta(desired.left, desired.right, monitor.left, monitor.right);
        const LONG dy = MoveClampDelta(desired.top, desired.bottom, monitor.top, monitor.bottom);
        result.left += dx;
        result.right += dx;
        result.top += dy;
        result.bottom += dy;
        return result;
    }
    // A resize only ever drags two edges, so only those are clamped. The
    // anchored two are left exactly where they were even if they are
    // outside the monitor -- a window that was already hanging off an
    // edge when the resize started keeps hanging off by the same amount,
    // rather than being quietly shoved on screen by a gesture the user
    // asked to change its size.
    if (GripMovesLeftEdge(grip)) {
        result.left = std::clamp(desired.left, monitor.left, monitor.right);
    } else {
        result.right = std::clamp(desired.right, monitor.left, monitor.right);
    }
    if (GripMovesTopEdge(grip)) {
        result.top = std::clamp(desired.top, monitor.top, monitor.bottom);
    } else {
        result.bottom = std::clamp(desired.bottom, monitor.top, monitor.bottom);
    }
    return result;
}

bool ShouldReleaseClamp(int pushedPastClampPx) {
    return pushedPastClampPx > kOvershootReleasePx;
}

RECT NextSnapInDirection(const RECT& current, const SnapCandidates& candidates, int dx, int dy) {
    RECT result = current;
    if (dx != 0 && !candidates.xEdges.empty()) {
        const LONG leading = dx > 0 ? current.right : current.left;
        const LONG delta = LeadingTarget(candidates.xEdges, leading, dx) - leading;
        result.left += delta;
        result.right += delta;
    }
    if (dy != 0 && !candidates.yEdges.empty()) {
        const LONG leading = dy > 0 ? current.bottom : current.top;
        const LONG delta = LeadingTarget(candidates.yEdges, leading, dy) - leading;
        result.top += delta;
        result.bottom += delta;
    }
    return result;
}

RECT EnforceMinimumSize(const RECT& desired, Grip grip, int minWidth, int minHeight) {
    if (grip == Grip::Move) {
        return desired;
    }
    RECT result = desired;
    if (result.right - result.left < minWidth) {
        if (GripMovesLeftEdge(grip)) {
            result.left = result.right - minWidth;
        } else {
            result.right = result.left + minWidth;
        }
    }
    if (result.bottom - result.top < minHeight) {
        if (GripMovesTopEdge(grip)) {
            result.top = result.bottom - minHeight;
        } else {
            result.bottom = result.top + minHeight;
        }
    }
    return result;
}

}  // namespace polish
