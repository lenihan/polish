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

}  // namespace

bool GripMovesLeft(Grip grip) {
    return grip == Grip::Left || grip == Grip::TopLeft || grip == Grip::BottomLeft;
}

bool GripMovesRight(Grip grip) {
    return grip == Grip::Right || grip == Grip::TopRight || grip == Grip::BottomRight;
}

bool GripMovesTop(Grip grip) {
    return grip == Grip::Top || grip == Grip::TopLeft || grip == Grip::TopRight;
}

bool GripMovesBottom(Grip grip) {
    return grip == Grip::Bottom || grip == Grip::BottomLeft || grip == Grip::BottomRight;
}

bool GripResizes(Grip grip) {
    return grip != Grip::Move;
}

int ResizeBorderPx(const RECT& visible, int requestedPx) {
    const LONG width = visible.right - visible.left;
    const LONG height = visible.bottom - visible.top;
    if (width <= 0 || height <= 0 || requestedPx <= 0) {
        return 0;
    }
    const LONG limit = std::min(width, height) * 3 / 10;
    return static_cast<int>(std::max<LONG>(0, std::min<LONG>(requestedPx, limit)));
}

Grip GripForPoint(const RECT& visible, POINT pt, int borderPx) {
    if (borderPx <= 0 || visible.right <= visible.left || visible.bottom <= visible.top) {
        return Grip::Move;
    }
    const bool left = pt.x < visible.left + borderPx;
    const bool right = pt.x >= visible.right - borderPx;
    const bool top = pt.y < visible.top + borderPx;
    const bool bottom = pt.y >= visible.bottom - borderPx;
    if (top && left) {
        return Grip::TopLeft;
    }
    if (top && right) {
        return Grip::TopRight;
    }
    if (bottom && left) {
        return Grip::BottomLeft;
    }
    if (bottom && right) {
        return Grip::BottomRight;
    }
    if (top) {
        return Grip::Top;
    }
    if (bottom) {
        return Grip::Bottom;
    }
    if (left) {
        return Grip::Left;
    }
    if (right) {
        return Grip::Right;
    }
    return Grip::Move;
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
    if (GripMovesLeft(grip)) {
        result.left += dx;
    }
    if (GripMovesRight(grip)) {
        result.right += dx;
    }
    if (GripMovesTop(grip)) {
        result.top += dy;
    }
    if (GripMovesBottom(grip)) {
        result.bottom += dy;
    }
    return result;
}

RECT ZoneRect(Grip grip, const RECT& visible, int borderPx) {
    const LONG b = static_cast<LONG>(std::max(0, borderPx));
    const LONG l = visible.left;
    const LONG t = visible.top;
    const LONG r = visible.right;
    const LONG bot = visible.bottom;
    if (b <= 0) {
        return visible;
    }
    switch (grip) {
        case Grip::Move:
            return RECT{l + b, t + b, r - b, bot - b};
        case Grip::Left:
            return RECT{l, t + b, l + b, bot - b};
        case Grip::Right:
            return RECT{r - b, t + b, r, bot - b};
        case Grip::Top:
            return RECT{l + b, t, r - b, t + b};
        case Grip::Bottom:
            return RECT{l + b, bot - b, r - b, bot};
        case Grip::TopLeft:
            return RECT{l, t, l + b, t + b};
        case Grip::TopRight:
            return RECT{r - b, t, r, t + b};
        case Grip::BottomLeft:
            return RECT{l, bot - b, l + b, bot};
        case Grip::BottomRight:
            return RECT{r - b, bot - b, r, bot};
        default:
            return visible;
    }
}

POINT ClampPointToRect(POINT pt, const RECT& r) {
    if (r.right <= r.left || r.bottom <= r.top) {
        return pt;
    }
    POINT out;
    out.x = std::clamp(pt.x, r.left, r.right - 1);
    out.y = std::clamp(pt.y, r.top, r.bottom - 1);
    return out;
}

SnapLayout LayoutForPointer(POINT pt, const RECT& monitor, int edgePx, int cornerPx) {
    if (monitor.right <= monitor.left || monitor.bottom <= monitor.top) {
        return SnapLayout::None;
    }
    // Outside the monitor entirely -- the pointer has moved on to another
    // screen, and this monitor has nothing to offer it.
    if (pt.x < monitor.left || pt.x >= monitor.right || pt.y < monitor.top || pt.y >= monitor.bottom) {
        return SnapLayout::None;
    }
    const bool nearLeftCorner = pt.x < monitor.left + cornerPx;
    const bool nearRightCorner = pt.x >= monitor.right - cornerPx;
    const bool nearTopCorner = pt.y < monitor.top + cornerPx;
    const bool nearBottomCorner = pt.y >= monitor.bottom - cornerPx;
    if (nearTopCorner && nearLeftCorner) {
        return SnapLayout::TopLeftQuarter;
    }
    if (nearTopCorner && nearRightCorner) {
        return SnapLayout::TopRightQuarter;
    }
    if (nearBottomCorner && nearLeftCorner) {
        return SnapLayout::BottomLeftQuarter;
    }
    if (nearBottomCorner && nearRightCorner) {
        return SnapLayout::BottomRightQuarter;
    }
    if (pt.y < monitor.top + edgePx) {
        return SnapLayout::Maximize;
    }
    if (pt.x < monitor.left + edgePx) {
        return SnapLayout::LeftHalf;
    }
    if (pt.x >= monitor.right - edgePx) {
        return SnapLayout::RightHalf;
    }
    return SnapLayout::None;
}

SnapLayout LayoutWithHysteresis(SnapLayout current, POINT pt, const RECT& monitor, int edgePx, int cornerPx,
                                int releasePx) {
    if (current == SnapLayout::None) {
        return LayoutForPointer(pt, monitor, edgePx, cornerPx);
    }
    // Already engaged: the same question, asked of a bigger zone. Staying
    // inside it keeps the layout, a different answer switches to it, and
    // only leaving the enlarged zone altogether lets go.
    return LayoutForPointer(pt, monitor, edgePx + releasePx, cornerPx + releasePx);
}

RECT RectForLayout(SnapLayout layout, const RECT& workArea) {
    const LONG midX = workArea.left + (workArea.right - workArea.left) / 2;
    const LONG midY = workArea.top + (workArea.bottom - workArea.top) / 2;
    switch (layout) {
        case SnapLayout::Maximize:
            return workArea;
        case SnapLayout::LeftHalf:
            return RECT{workArea.left, workArea.top, midX, workArea.bottom};
        case SnapLayout::RightHalf:
            return RECT{midX, workArea.top, workArea.right, workArea.bottom};
        case SnapLayout::TopLeftQuarter:
            return RECT{workArea.left, workArea.top, midX, midY};
        case SnapLayout::TopRightQuarter:
            return RECT{midX, workArea.top, workArea.right, midY};
        case SnapLayout::BottomLeftQuarter:
            return RECT{workArea.left, midY, midX, workArea.bottom};
        case SnapLayout::BottomRightQuarter:
            return RECT{midX, midY, workArea.right, workArea.bottom};
        case SnapLayout::None:
        default:
            return workArea;
    }
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
    if (GripMovesLeft(grip)) {
        result.left += BestDelta(candidates.xEdges, desired.left, thresholdPx);
    }
    if (GripMovesRight(grip)) {
        result.right += BestDelta(candidates.xEdges, desired.right, thresholdPx);
    }
    if (GripMovesTop(grip)) {
        result.top += BestDelta(candidates.yEdges, desired.top, thresholdPx);
    }
    if (GripMovesBottom(grip)) {
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
    // A resize only ever drags one or two edges, so only those are
    // clamped. The
    // anchored two are left exactly where they were even if they are
    // outside the monitor -- a window that was already hanging off an
    // edge when the resize started keeps hanging off by the same amount,
    // rather than being quietly shoved on screen by a gesture the user
    // asked to change its size.
    if (GripMovesLeft(grip)) {
        result.left = std::clamp(desired.left, monitor.left, monitor.right);
    }
    if (GripMovesRight(grip)) {
        result.right = std::clamp(desired.right, monitor.left, monitor.right);
    }
    if (GripMovesTop(grip)) {
        result.top = std::clamp(desired.top, monitor.top, monitor.bottom);
    }
    if (GripMovesBottom(grip)) {
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
        // Whichever horizontal edge this grip drags gets pushed back. A
        // grip that drags neither (Top, Bottom) cannot have changed the
        // width, so there is nothing to correct.
        if (GripMovesLeft(grip)) {
            result.left = result.right - minWidth;
        } else if (GripMovesRight(grip)) {
            result.right = result.left + minWidth;
        }
    }
    if (result.bottom - result.top < minHeight) {
        if (GripMovesTop(grip)) {
            result.top = result.bottom - minHeight;
        } else if (GripMovesBottom(grip)) {
            result.bottom = result.top + minHeight;
        }
    }
    return result;
}

}  // namespace polish
