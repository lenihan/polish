#include "windowtracking/StackStripLayout.h"

#include <algorithm>

namespace polish {

std::vector<RECT> TabRects(const RECT& strip, int tabCount, StackAlignment alignment, const TabMetrics& metrics) {
    std::vector<RECT> rects;
    if (tabCount <= 0 || strip.right <= strip.left || strip.bottom <= strip.top) {
        return rects;
    }

    if (alignment == StackAlignment::Vertical) {
        const int left = strip.left + metrics.crossPad;
        const int right = strip.right - metrics.crossPad;
        if (right <= left) {
            return rects;
        }
        const int limit = strip.bottom - metrics.leadingPad;
        int y = strip.top + metrics.leadingPad;
        for (int i = 0; i < tabCount && y < limit; ++i) {
            rects.push_back(RECT{left, y, right, std::min(y + metrics.rowHeight, limit)});
            y += metrics.rowHeight + metrics.gap;
        }
        return rects;
    }

    const int top = strip.top + metrics.crossPad;
    const int bottom = strip.bottom - metrics.crossPad;
    if (bottom <= top) {
        return rects;
    }
    const int limit = strip.right - metrics.leadingPad;
    const int available = (strip.right - strip.left) - 2 * metrics.leadingPad - (tabCount - 1) * metrics.gap;
    const int width = std::clamp(available / tabCount, metrics.floorWidth, metrics.maxWidth);
    int x = strip.left + metrics.leadingPad;
    for (int i = 0; i < tabCount && x < limit; ++i) {
        const int right = x + width;
        if (right > limit && i > 0 && limit - x < metrics.floorWidth) {
            // What is left is too narrow to be a usable tab.
            break;
        }
        rects.push_back(RECT{x, top, std::min(right, limit), bottom});
        x += width + metrics.gap;
    }
    return rects;
}

std::optional<size_t> TabIndexAt(const std::vector<RECT>& rects, POINT pt) {
    for (size_t i = 0; i < rects.size(); ++i) {
        const RECT& r = rects[i];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
            return i;
        }
    }
    return std::nullopt;
}

size_t ReorderTargetIndex(const std::vector<RECT>& rects, size_t dragged, POINT pt, StackAlignment alignment) {
    if (rects.empty()) {
        return dragged;
    }
    if (const auto over = TabIndexAt(rects, pt)) {
        return *over;
    }
    const bool horizontal = alignment == StackAlignment::Horizontal;
    const LONG coordinate = horizontal ? pt.x : pt.y;
    const LONG first = horizontal ? rects.front().left : rects.front().top;
    const LONG last = horizontal ? rects.back().right : rects.back().bottom;
    if (coordinate < first) {
        return 0;
    }
    if (coordinate >= last) {
        return rects.size() - 1;
    }
    return dragged;  // in a gap between tabs
}

bool DragTearsOut(const RECT& strip, POINT pt, int thresholdPx) {
    return pt.x < strip.left - thresholdPx || pt.x >= strip.right + thresholdPx || pt.y < strip.top - thresholdPx ||
           pt.y >= strip.bottom + thresholdPx;
}

size_t InsertionIndexAt(const std::vector<RECT>& rects, POINT pt, StackAlignment alignment) {
    const bool horizontal = alignment == StackAlignment::Horizontal;
    const LONG coordinate = horizontal ? pt.x : pt.y;
    for (size_t i = 0; i < rects.size(); ++i) {
        const LONG mid = horizontal ? (rects[i].left + rects[i].right) / 2 : (rects[i].top + rects[i].bottom) / 2;
        if (coordinate < mid) {
            return i;
        }
    }
    return rects.size();
}

}  // namespace polish
