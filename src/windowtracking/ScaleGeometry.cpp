#include "windowtracking/ScaleGeometry.h"

#include <algorithm>
#include <cmath>

namespace polish {

namespace {

constexpr double kEpsilon = 1e-6;

LONG RoundToLong(double value) {
    return static_cast<LONG>(std::lround(value));
}

// `rect` shifted into `work`, size untouched. Right/bottom first and
// left/top second, so a rect too big for `work` ends up pinned to the
// top-left rather than the bottom-right.
RECT ShiftInto(RECT rect, const RECT& work) {
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (rect.right > work.right) {
        rect.left = work.right - width;
    }
    if (rect.bottom > work.bottom) {
        rect.top = work.bottom - height;
    }
    if (rect.left < work.left) {
        rect.left = work.left;
    }
    if (rect.top < work.top) {
        rect.top = work.top;
    }
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

}  // namespace

SIZE ScaledSize(SIZE real, double scale) {
    return {std::max<LONG>(1, RoundToLong(real.cx * scale)), std::max<LONG>(1, RoundToLong(real.cy * scale))};
}

RECT ScaledViewRect(SIZE real, double scale, POINT topLeft) {
    const SIZE size = ScaledSize(real, scale);
    return {topLeft.x, topLeft.y, topLeft.x + size.cx, topLeft.y + size.cy};
}

double ScaleOf(const RECT& view, SIZE real) {
    if (real.cx <= 0) {
        return kMaxScale;
    }
    return static_cast<double>(view.right - view.left) / static_cast<double>(real.cx);
}

RECT UnscaledRectAt(SIZE real, POINT viewTopLeft, const RECT& work) {
    return ShiftInto({viewTopLeft.x, viewTopLeft.y, viewTopLeft.x + real.cx, viewTopLeft.y + real.cy}, work);
}

RECT ClampViewToWorkArea(const RECT& view, const RECT& work) {
    return ShiftInto(view, work);
}

RECT CornerDragAspectLocked(const RECT& start, Corner grabbed, POINT pointer, SIZE real, double minScale,
                            double maxScale) {
    // The corner that does not move, and which way the view extends from
    // it: +1 grows right/down from the anchor, -1 left/up.
    const bool growsLeft = grabbed == Corner::TopLeft || grabbed == Corner::BottomLeft;
    const bool growsUp = grabbed == Corner::TopLeft || grabbed == Corner::TopRight;
    const LONG anchorX = growsLeft ? start.right : start.left;
    const LONG anchorY = growsUp ? start.bottom : start.top;

    // Signed distance from the anchor along each growth direction;
    // negative means the pointer crossed over the anchor.
    const double dx = growsLeft ? static_cast<double>(anchorX - pointer.x) : static_cast<double>(pointer.x - anchorX);
    const double dy = growsUp ? static_cast<double>(anchorY - pointer.y) : static_cast<double>(pointer.y - anchorY);

    double scale = minScale;
    if (real.cx > 0 && real.cy > 0) {
        const double fromX = dx / static_cast<double>(real.cx);
        const double fromY = dy / static_cast<double>(real.cy);
        scale = std::max(fromX, fromY);
    }
    scale = std::clamp(scale, minScale, maxScale);

    const SIZE size = ScaledSize(real, scale);
    RECT result;
    result.left = growsLeft ? anchorX - size.cx : anchorX;
    result.right = result.left + size.cx;
    result.top = growsUp ? anchorY - size.cy : anchorY;
    result.bottom = result.top + size.cy;
    return result;
}

double SnapScale(double scale, double tolerance) {
    if (std::fabs(scale - kMaxScale) <= tolerance) {
        return kMaxScale;
    }
    double best = scale;
    double bestDistance = tolerance + kEpsilon;
    for (const double preset : kScalePresets) {
        const double distance = std::fabs(scale - preset);
        if (distance < bestDistance) {
            best = preset;
            bestDistance = distance;
        }
    }
    return best;
}

std::optional<double> StepScale(double current, int direction) {
    if (direction == 0) {
        return current;
    }
    if (direction < 0) {
        // Largest preset strictly below `current`.
        for (const double preset : kScalePresets) {
            if (preset < current - kEpsilon) {
                return preset;
            }
        }
        return current;  // already at or below the smallest preset
    }
    // Smallest preset strictly above `current`.
    for (int i = kScalePresetCount - 1; i >= 0; --i) {
        if (kScalePresets[i] > current + kEpsilon) {
            return kScalePresets[i];
        }
    }
    return std::nullopt;
}

RECT ResizeViewKeepingAnchor(const RECT& view, double scale, SIZE newReal) {
    return ScaledViewRect(newReal, scale, {view.left, view.top});
}

}  // namespace polish
