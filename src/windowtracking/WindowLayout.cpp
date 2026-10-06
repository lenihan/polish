#include "windowtracking/WindowLayout.h"

#include <algorithm>

namespace polish {

int SlotsFor(ArrangeKind kind) {
    switch (kind) {
        case ArrangeKind::TwoWay:
            return 2;
        case ArrangeKind::ThreeWay:
            return 3;
        case ArrangeKind::FourWay:
            return 4;
    }
    return 0;
}

Orientation OrientationOf(const RECT& work) {
    const LONG width = work.right - work.left;
    const LONG height = work.bottom - work.top;
    return width >= height ? Orientation::Landscape : Orientation::Portrait;
}

std::vector<RECT> SliceAlongLongAxis(const RECT& work, int n) {
    std::vector<RECT> rects;
    if (n <= 0 || work.right <= work.left || work.bottom <= work.top) {
        return rects;
    }
    rects.reserve(static_cast<size_t>(n));

    if (OrientationOf(work) == Orientation::Landscape) {
        const LONG width = work.right - work.left;
        // Computed from the index rather than accumulated, and shared
        // between neighbouring slices, so there is no seam and no lost
        // pixel -- edge(n) is exactly work.right.
        const auto edge = [&](int i) { return work.left + width * i / n; };
        for (int i = 0; i < n; ++i) {
            rects.push_back(RECT{edge(i), work.top, edge(i + 1), work.bottom});
        }
    } else {
        const LONG height = work.bottom - work.top;
        const auto edge = [&](int i) { return work.top + height * i / n; };
        for (int i = 0; i < n; ++i) {
            rects.push_back(RECT{work.left, edge(i), work.right, edge(i + 1)});
        }
    }
    return rects;
}

std::vector<RECT> ArrangeRects(const RECT& work, ArrangeKind kind, int count) {
    if (count != SlotsFor(kind) || work.right <= work.left || work.bottom <= work.top) {
        return {};
    }

    switch (kind) {
        case ArrangeKind::TwoWay:
        case ArrangeKind::ThreeWay:
            return SliceAlongLongAxis(work, count);

        case ArrangeKind::FourWay: {
            // Always a 2x2, in both orientations: the four corners are the
            // same four corners whichever way round the monitor is, and
            // four quarters is what "4-way" means to everyone who has used
            // another window manager.
            //
            // Deliberately not "slice in two, then slice each half in two
            // along its own long axis". That is tidier to write and gives
            // four quarters on an ordinary monitor, but on an ultrawide
            // (3840x1080, say) each half is still landscape, so it would
            // slice again the same way and produce four columns instead.
            // Four columns may well be the better layout there, but it is
            // not what this command is called, and a command whose shape
            // depends on the aspect ratio in a way nobody can predict is
            // worse than one that is merely suboptimal.
            //
            // Both midpoints are shared between the slots either side of
            // them, for the same no-seam reason as SliceAlongLongAxis.
            const LONG midX = work.left + (work.right - work.left) / 2;
            const LONG midY = work.top + (work.bottom - work.top) / 2;
            return {RECT{work.left, work.top, midX, midY},
                    RECT{midX, work.top, work.right, midY},
                    RECT{work.left, midY, midX, work.bottom},
                    RECT{midX, midY, work.right, work.bottom}};
        }
    }
    return {};
}


std::vector<int> DistributeWithMinimums(int total, int n, const std::vector<int>& minimums) {
    std::vector<int> sizes;
    if (n <= 0 || total <= 0) {
        return sizes;
    }
    std::vector<int> mins(static_cast<size_t>(n), 0);
    long long minSum = 0;
    for (int i = 0; i < n && i < static_cast<int>(minimums.size()); ++i) {
        mins[static_cast<size_t>(i)] = std::max(0, minimums[static_cast<size_t>(i)]);
    }
    for (int m : mins) {
        minSum += m;
    }
    if (minSum > total) {
        // The minimums cannot all be honoured, by a little (CheckArrangeFit
        // allows a small shortfall) or by a lot (a caller that did not
        // check). Scale every slot down in proportion instead of dropping
        // the minimums, so a window that needs more still gets more.
        //
        // Each weight has a floor, so a window with no minimum at all is
        // not squeezed to nothing by one that has a large one.
        const long long floorWeight = std::max<long long>(1, total / (2LL * n));
        std::vector<long long> weight(static_cast<size_t>(n));
        long long weightSum = 0;
        for (int i = 0; i < n; ++i) {
            weight[static_cast<size_t>(i)] = std::max<long long>(mins[static_cast<size_t>(i)], floorWeight);
            weightSum += weight[static_cast<size_t>(i)];
        }
        sizes.assign(static_cast<size_t>(n), 0);
        long long before = 0;
        long long cumulative = 0;
        for (int i = 0; i < n; ++i) {
            cumulative += weight[static_cast<size_t>(i)];
            const long long edge = static_cast<long long>(total) * cumulative / weightSum;
            sizes[static_cast<size_t>(i)] = static_cast<int>(edge - before);
            before = edge;
        }
        return sizes;
    }

    // Slots held at their minimum, found by repeated narrowing: fixing one
    // lowers everyone else's share, which can push another over its limit.
    std::vector<bool> fixed(static_cast<size_t>(n), false);
    long long fixedSum = 0;
    int freeCount = n;
    for (bool changed = true; changed && freeCount > 0;) {
        changed = false;
        const long long share = (total - fixedSum) / freeCount;
        for (int i = 0; i < n; ++i) {
            if (!fixed[static_cast<size_t>(i)] && mins[static_cast<size_t>(i)] > share) {
                fixed[static_cast<size_t>(i)] = true;
                fixedSum += mins[static_cast<size_t>(i)];
                --freeCount;
                changed = true;
            }
        }
    }

    // The free slots divide the remainder. Boundaries come from a running
    // index, not from repeated rounding, so the sizes sum exactly.
    const long long remaining = total - fixedSum;
    sizes.assign(static_cast<size_t>(n), 0);
    int k = 0;
    long long used = 0;
    for (int i = 0; i < n; ++i) {
        if (fixed[static_cast<size_t>(i)]) {
            sizes[static_cast<size_t>(i)] = mins[static_cast<size_t>(i)];
        } else {
            const long long a = remaining * k / freeCount;
            const long long b = remaining * (k + 1) / freeCount;
            sizes[static_cast<size_t>(i)] = static_cast<int>(b - a);
            ++k;
        }
        used += sizes[static_cast<size_t>(i)];
    }
    // Only reachable if every slot was fixed with room to spare; give the
    // spare to the last slot so the total still holds.
    if (used < total) {
        sizes.back() += static_cast<int>(total - used);
    }
    return sizes;
}

std::vector<RECT> ArrangeRectsWithMinimums(const RECT& work, ArrangeKind kind, int count,
                                           const std::vector<SIZE>& minSizes) {
    if (minSizes.empty() || static_cast<int>(minSizes.size()) != count) {
        return ArrangeRects(work, kind, count);
    }
    if (count != SlotsFor(kind) || work.right <= work.left || work.bottom <= work.top) {
        return {};
    }
    const int width = work.right - work.left;
    const int height = work.bottom - work.top;

    if (kind == ArrangeKind::FourWay) {
        // Slots are [TL, TR, BL, BR]. A column is as wide as the widest
        // minimum in it, a row as tall as the tallest, so the four still
        // tile exactly whichever window is the stubborn one.
        const std::vector<int> colMin = {
            std::max<int>(minSizes[0].cx, minSizes[2].cx),
            std::max<int>(minSizes[1].cx, minSizes[3].cx),
        };
        const std::vector<int> rowMin = {
            std::max<int>(minSizes[0].cy, minSizes[1].cy),
            std::max<int>(minSizes[2].cy, minSizes[3].cy),
        };
        const std::vector<int> cols = DistributeWithMinimums(width, 2, colMin);
        const std::vector<int> rows = DistributeWithMinimums(height, 2, rowMin);
        const LONG midX = work.left + cols[0];
        const LONG midY = work.top + rows[0];
        return {RECT{work.left, work.top, midX, midY}, RECT{midX, work.top, work.right, midY},
                RECT{work.left, midY, midX, work.bottom}, RECT{midX, midY, work.right, work.bottom}};
    }

    // 2-way and 3-way: slices along the long axis.
    const bool landscape = OrientationOf(work) == Orientation::Landscape;
    std::vector<int> axisMin;
    axisMin.reserve(minSizes.size());
    for (const SIZE& s : minSizes) {
        axisMin.push_back(landscape ? s.cx : s.cy);
    }
    const std::vector<int> sizes = DistributeWithMinimums(landscape ? width : height, count, axisMin);

    std::vector<RECT> rects;
    rects.reserve(static_cast<size_t>(count));
    LONG edge = landscape ? work.left : work.top;
    for (int i = 0; i < count; ++i) {
        const LONG next = edge + sizes[static_cast<size_t>(i)];
        rects.push_back(landscape ? RECT{edge, work.top, next, work.bottom}
                                  : RECT{work.left, edge, work.right, next});
        edge = next;
    }
    return rects;
}

namespace {

// How far over its length a set of minimums may run and still count as// fitting: one percent, and never under 12px.//// Without this, a shortfall of a few pixels refuses the command outright.// That happened: Outlook needs 1286px of height and VS Code windows 542px// each, so two rows need 1828px on a 1824px work area. Four pixels is not// something a person can see, and "can't tile, 4px short" is a worse// answer than a layout in which two windows overlap by 4px.//// It also absorbs a known inaccuracy: a maximized window's minimum is// measured without its invisible border being removed (see// MinimumVisibleSizeFor in main.cpp), which overstates it by about ten// pixels. Anything genuinely too big -- tens of pixels past this -- is// still refused. The slots are scaled down proportionally to absorb the// shortfall (DistributeWithMinimums), so each stubborn window overruns// its slot by about the same, small, amount.
int FitSlackPx(int available) {
    return std::max(12, available / 100);
}

}  // namespace

ArrangeFit CheckArrangeFit(const RECT& work, ArrangeKind kind, int count, const std::vector<SIZE>& minSizes) {
    ArrangeFit ok;
    if (minSizes.empty() || static_cast<int>(minSizes.size()) != count || count != SlotsFor(kind) ||
        work.right <= work.left || work.bottom <= work.top) {
        return ok;
    }
    const int width = work.right - work.left;
    const int height = work.bottom - work.top;

    const auto fail = [](int blocker, bool tooWide, int needed, int available, int blockerMin) {
        ArrangeFit f;
        f.fits = false;
        f.blocker = blocker;
        f.tooWide = tooWide;
        f.needed = needed;
        f.available = available;
        f.blockerMin = blockerMin;
        return f;
    };
    // Index of the window with the largest value of `pick`.
    const auto largest = [&](int (*pick)(const SIZE&)) {
        int best = 0;
        for (int i = 1; i < count; ++i) {
            if (pick(minSizes[static_cast<size_t>(i)]) > pick(minSizes[static_cast<size_t>(best)])) {
                best = i;
            }
        }
        return best;
    };
    const auto cx = [](const SIZE& s) { return static_cast<int>(s.cx); };
    const auto cy = [](const SIZE& s) { return static_cast<int>(s.cy); };

    if (kind == ArrangeKind::FourWay) {
        const int colNeed = std::max<int>(minSizes[0].cx, minSizes[2].cx) + std::max<int>(minSizes[1].cx, minSizes[3].cx);
        if (colNeed > width + FitSlackPx(width)) {
            const int b = largest(+cx);
            return fail(b, true, colNeed, width, minSizes[static_cast<size_t>(b)].cx);
        }
        const int rowNeed = std::max<int>(minSizes[0].cy, minSizes[1].cy) + std::max<int>(minSizes[2].cy, minSizes[3].cy);
        if (rowNeed > height + FitSlackPx(height)) {
            const int b = largest(+cy);
            return fail(b, false, rowNeed, height, minSizes[static_cast<size_t>(b)].cy);
        }
        return ok;
    }

    // 2-way and 3-way: slices along the long axis, full length across it.
    const bool landscape = OrientationOf(work) == Orientation::Landscape;
    long long axisNeed = 0;
    for (const SIZE& s : minSizes) {
        axisNeed += landscape ? s.cx : s.cy;
    }
    const int axisLen = landscape ? width : height;
    if (axisNeed > axisLen + FitSlackPx(axisLen)) {
        const int b = landscape ? largest(+cx) : largest(+cy);
        return fail(b, landscape, static_cast<int>(axisNeed), axisLen,
                    static_cast<int>(landscape ? minSizes[static_cast<size_t>(b)].cx : minSizes[static_cast<size_t>(b)].cy));
    }
    // Across the long axis each window gets the whole other dimension.
    const int crossLen = landscape ? height : width;
    for (int i = 0; i < count; ++i) {
        const int m = landscape ? minSizes[static_cast<size_t>(i)].cy : minSizes[static_cast<size_t>(i)].cx;
        if (m > crossLen + FitSlackPx(crossLen)) {
            return fail(i, !landscape, m, crossLen, m);
        }
    }
    return ok;
}

ArrangeFit CheckArrangeFitAnyOrder(const RECT& work, ArrangeKind kind, const std::vector<SIZE>& minSizes) {
    const int n = SlotsFor(kind);
    const ArrangeFit first = CheckArrangeFit(work, kind, n, minSizes);
    if (first.fits || static_cast<int>(minSizes.size()) != n) {
        return first;
    }
    // Every assignment of windows to slots: at most 4! = 24, so trying them
    // all is cheaper than being clever about which could work.
    std::vector<int> order(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        order[static_cast<size_t>(i)] = i;
    }
    std::vector<SIZE> arranged(static_cast<size_t>(n));
    while (std::next_permutation(order.begin(), order.end())) {
        for (int slot = 0; slot < n; ++slot) {
            arranged[static_cast<size_t>(slot)] = minSizes[static_cast<size_t>(order[static_cast<size_t>(slot)])];
        }
        if (CheckArrangeFit(work, kind, n, arranged).fits) {
            return ArrangeFit{};
        }
    }
    return first;
}

}  // namespace polish
