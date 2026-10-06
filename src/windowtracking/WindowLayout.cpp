#include "windowtracking/WindowLayout.h"

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

}  // namespace polish
