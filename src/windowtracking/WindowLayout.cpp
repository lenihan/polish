#include "windowtracking/WindowLayout.h"

#include <algorithm>
#include <cmath>

namespace polish {

LayoutGrid TileShape(int count) {
    if (count <= 0) {
        return LayoutGrid{0, 0};
    }
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + cols - 1) / cols;
    return LayoutGrid{cols, rows};
}

std::vector<RECT> TileRects(const RECT& work, int count) {
    std::vector<RECT> rects;
    if (count <= 0 || work.right <= work.left || work.bottom <= work.top) {
        return rects;
    }
    rects.reserve(static_cast<size_t>(count));

    const LayoutGrid grid = TileShape(count);
    const LONG width = work.right - work.left;
    const LONG height = work.bottom - work.top;

    // How many rows are completely full, and what is left over for a
    // final short row.
    const int fullRows = count / grid.cols;
    const int remainder = count % grid.cols;

    // Row edges computed from the row index rather than accumulated, so
    // rounding cannot drift and leave a seam between rows.
    const auto rowTop = [&](int row) { return work.top + height * row / grid.rows; };

    for (int row = 0; row < fullRows; ++row) {
        for (int col = 0; col < grid.cols; ++col) {
            rects.push_back(RECT{work.left + width * col / grid.cols, rowTop(row),
                                 work.left + width * (col + 1) / grid.cols, rowTop(row + 1)});
        }
    }
    if (remainder > 0) {
        // The short row shares the full width between however many are
        // left, so there is no hole beside them.
        const int row = fullRows;
        for (int col = 0; col < remainder; ++col) {
            rects.push_back(RECT{work.left + width * col / remainder, rowTop(row),
                                 work.left + width * (col + 1) / remainder, rowTop(row + 1)});
        }
    }
    return rects;
}

std::vector<RECT> CascadeRects(const RECT& work, int count, int stepPx) {
    std::vector<RECT> rects;
    if (count <= 0 || work.right <= work.left || work.bottom <= work.top) {
        return rects;
    }
    rects.reserve(static_cast<size_t>(count));

    const LONG width = work.right - work.left;
    const LONG height = work.bottom - work.top;
    const LONG winWidth = std::max<LONG>(1, width * kCascadeWidthPercent / 100);
    const LONG winHeight = std::max<LONG>(1, height * kCascadeHeightPercent / 100);
    const LONG step = std::max<LONG>(1, stepPx);

    // How many windows fit before one would hang off the work area. At
    // least one, so a step larger than the leftover room still produces
    // a stack rather than nothing.
    const LONG slotsAcross = (width - winWidth) / step;
    const LONG slotsDown = (height - winHeight) / step;
    const LONG slots = std::max<LONG>(1, std::min(slotsAcross, slotsDown) + 1);

    for (int i = 0; i < count; ++i) {
        const LONG slot = static_cast<LONG>(i) % slots;
        const LONG left = work.left + slot * step;
        const LONG top = work.top + slot * step;
        rects.push_back(RECT{left, top, left + winWidth, top + winHeight});
    }
    return rects;
}

}  // namespace polish
