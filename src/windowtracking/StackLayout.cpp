#include "windowtracking/StackLayout.h"

#include <algorithm>

namespace polish {

StackFrame ComputeStackFrame(const RECT& stackRect, StackAlignment alignment, int stripThicknessPx) {
    StackFrame frame{};
    const LONG width = stackRect.right - stackRect.left;
    const LONG height = stackRect.bottom - stackRect.top;
    if (width <= 0 || height <= 0) {
        // Nothing to divide. Both parts are the (empty) rect itself.
        frame.strip = stackRect;
        frame.content = stackRect;
        return frame;
    }

    // Never take the whole of the dimension: the content keeps at least a
    // pixel, so it is a real rect rather than an empty one a caller would
    // have to special-case.
    const int thickness = std::clamp(stripThicknessPx, 0, static_cast<int>((alignment == StackAlignment::Horizontal ? height : width) - 1));

    if (alignment == StackAlignment::Horizontal) {
        frame.strip = RECT{stackRect.left, stackRect.top, stackRect.right, stackRect.top + thickness};
        frame.content = RECT{stackRect.left, stackRect.top + thickness, stackRect.right, stackRect.bottom};
    } else {
        frame.strip = RECT{stackRect.left, stackRect.top, stackRect.left + thickness, stackRect.bottom};
        frame.content = RECT{stackRect.left + thickness, stackRect.top, stackRect.right, stackRect.bottom};
    }
    return frame;
}

RECT StackRectFromContent(const RECT& content, StackAlignment alignment, int stripThicknessPx) {
    const int thickness = std::max(0, stripThicknessPx);
    if (alignment == StackAlignment::Horizontal) {
        return RECT{content.left, content.top - thickness, content.right, content.bottom};
    }
    return RECT{content.left - thickness, content.top, content.right, content.bottom};
}

}  // namespace polish
