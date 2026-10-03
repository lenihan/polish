#include "windowtracking/MoveSnap.h"

#include <doctest/doctest.h>

#include <array>
#include <vector>

using namespace polish;

namespace {

// A plain 1920x1080 monitor whose taskbar eats the bottom 40px, which is
// what most of these cases snap against.
constexpr RECT kMonitor = {0, 0, 1920, 1080};
constexpr RECT kWorkArea = {0, 0, 1920, 1040};

bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

SnapCandidates WorkAreaOnly() {
    return CollectSnapEdges(kWorkArea, {});
}

}  // namespace

TEST_CASE("ResizeBorderPx: the full band on a big window, shrunk on a small one") {
    constexpr RECT big = {0, 0, 2000, 1500};
    CHECK(ResizeBorderPx(big, 192) == 192);

    // 30% of the smaller side, so a band can never eat the move zone.
    constexpr RECT small = {0, 0, 400, 300};
    CHECK(ResizeBorderPx(small, 192) == 90);

    // Degenerate inputs give no band at all, making the whole window a
    // move target -- the safe way round.
    CHECK(ResizeBorderPx(RECT{0, 0, 0, 0}, 192) == 0);
    CHECK(ResizeBorderPx(big, 0) == 0);
}

TEST_CASE("GripForPoint: the band resizes and the middle moves") {
    constexpr RECT window = {100, 100, 1100, 800};  // 1000 x 700
    constexpr int border = 100;

    CHECK(GripForPoint(window, {600, 450}, border) == Grip::Move);  // dead centre
    CHECK(GripForPoint(window, {110, 450}, border) == Grip::Left);
    CHECK(GripForPoint(window, {1090, 450}, border) == Grip::Right);
    CHECK(GripForPoint(window, {600, 110}, border) == Grip::Top);
    CHECK(GripForPoint(window, {600, 790}, border) == Grip::Bottom);
    CHECK(GripForPoint(window, {110, 110}, border) == Grip::TopLeft);
    CHECK(GripForPoint(window, {1090, 110}, border) == Grip::TopRight);
    CHECK(GripForPoint(window, {110, 790}, border) == Grip::BottomLeft);
    CHECK(GripForPoint(window, {1090, 790}, border) == Grip::BottomRight);
}

TEST_CASE("GripForPoint: the boundary belongs to the band, and just inside it moves") {
    constexpr RECT window = {100, 100, 1100, 800};
    constexpr int border = 100;
    CHECK(GripForPoint(window, {199, 450}, border) == Grip::Left);  // last band pixel
    CHECK(GripForPoint(window, {200, 450}, border) == Grip::Move);  // first move pixel
    CHECK(GripForPoint(window, {1000, 450}, border) == Grip::Right);
    CHECK(GripForPoint(window, {999, 450}, border) == Grip::Move);
}

TEST_CASE("GripForPoint: no band, or a degenerate window, is all move") {
    constexpr RECT window = {100, 100, 1100, 800};
    CHECK(GripForPoint(window, {100, 100}, 0) == Grip::Move);
    CHECK(GripForPoint(RECT{5, 5, 5, 5}, {5, 5}, 100) == Grip::Move);
}

TEST_CASE("Grip edge predicates: each grip drags exactly the edges it names") {
    CHECK_FALSE(GripMovesLeft(Grip::Move));
    CHECK_FALSE(GripMovesRight(Grip::Move));
    CHECK_FALSE(GripMovesTop(Grip::Move));
    CHECK_FALSE(GripMovesBottom(Grip::Move));
    CHECK_FALSE(GripResizes(Grip::Move));

    CHECK(GripMovesLeft(Grip::Left));
    CHECK_FALSE(GripMovesRight(Grip::Left));
    CHECK_FALSE(GripMovesTop(Grip::Left));
    CHECK_FALSE(GripMovesBottom(Grip::Left));

    CHECK(GripMovesTop(Grip::TopRight));
    CHECK(GripMovesRight(Grip::TopRight));
    CHECK_FALSE(GripMovesLeft(Grip::TopRight));
    CHECK_FALSE(GripMovesBottom(Grip::TopRight));
    CHECK(GripResizes(Grip::TopRight));
}

TEST_CASE("DragGrip: a move displaces the whole rect, a corner only its own two edges") {
    constexpr RECT start = {100, 100, 500, 400};

    CHECK(Same(DragGrip(start, Grip::Move, 30, -20), RECT{130, 80, 530, 380}));

    // Each corner leaves the opposite two edges exactly where they were.
    CHECK(Same(DragGrip(start, Grip::TopLeft, 30, -20), RECT{130, 80, 500, 400}));
    CHECK(Same(DragGrip(start, Grip::TopRight, 30, -20), RECT{100, 80, 530, 400}));
    CHECK(Same(DragGrip(start, Grip::BottomLeft, 30, -20), RECT{130, 100, 500, 380}));
    CHECK(Same(DragGrip(start, Grip::BottomRight, 30, -20), RECT{100, 100, 530, 380}));

    // An edge grip moves one edge and ignores the other axis entirely.
    CHECK(Same(DragGrip(start, Grip::Left, 30, -20), RECT{130, 100, 500, 400}));
    CHECK(Same(DragGrip(start, Grip::Right, 30, -20), RECT{100, 100, 530, 400}));
    CHECK(Same(DragGrip(start, Grip::Top, 30, -20), RECT{100, 80, 500, 400}));
    CHECK(Same(DragGrip(start, Grip::Bottom, 30, -20), RECT{100, 100, 500, 380}));
}

TEST_CASE("CollectSnapEdges: work-area edges are always candidates, sorted and deduplicated") {
    const std::array<RECT, 2> others = {RECT{0, 200, 400, 600}, RECT{0, 700, 400, 900}};
    const SnapCandidates candidates = CollectSnapEdges(kWorkArea, others);
    // 0 appears as the work area's left and as both neighbours' left, once.
    CHECK(candidates.xEdges == std::vector<LONG>{0, 400, 1920});
    CHECK(candidates.yEdges == std::vector<LONG>{0, 200, 600, 700, 900, 1040});
}

TEST_CASE("ApplySnap: a move within the threshold is pulled flush to the work-area edge") {
    constexpr RECT nearLeft = {8, 300, 508, 700};
    const RECT snapped = ApplySnap(nearLeft, WorkAreaOnly(), kDefaultSnapThresholdPx, Grip::Move);
    CHECK(Same(snapped, RECT{0, 300, 500, 700}));  // size preserved
}

TEST_CASE("ApplySnap: the snap is magnetic, not blocking -- past the threshold nothing moves") {
    // What makes overlapping windows need no modifier: exceed the
    // threshold and the attraction simply stops.
    const SnapCandidates candidates = WorkAreaOnly();
    constexpr RECT atThreshold = {kDefaultSnapThresholdPx, 300, 512, 700};
    CHECK(Same(ApplySnap(atThreshold, candidates, kDefaultSnapThresholdPx, Grip::Move),
               RECT{0, 300, 500, 700}));
    constexpr RECT pastThreshold = {kDefaultSnapThresholdPx + 1, 300, 513, 700};
    CHECK(Same(ApplySnap(pastThreshold, candidates, kDefaultSnapThresholdPx, Grip::Move), pastThreshold));
}

TEST_CASE("ApplySnap: a neighbour gives both edge-to-edge butting and alignment") {
    const std::array<RECT, 1> others = {RECT{600, 200, 1000, 800}};
    const SnapCandidates candidates = CollectSnapEdges(kWorkArea, others);

    // Butting: the dragged window's left edge finds the neighbour's right.
    constexpr RECT nearButt = {1004, 300, 1304, 600};
    CHECK(Same(ApplySnap(nearButt, candidates, kDefaultSnapThresholdPx, Grip::Move),
               RECT{1000, 300, 1300, 600}));

    // Alignment: its left edge finds the neighbour's left.
    constexpr RECT nearAlign = {595, 850, 895, 1000};
    CHECK(Same(ApplySnap(nearAlign, candidates, kDefaultSnapThresholdPx, Grip::Move),
               RECT{600, 850, 900, 1000}));
}

TEST_CASE("ApplySnap: the closer of the two edges wins an axis") {
    const std::array<RECT, 1> others = {RECT{0, 0, 500, 1040}};
    const SnapCandidates candidates = CollectSnapEdges(kWorkArea, others);
    // left is 6px from 500, right is 3px from 1920 -- the right edge wins.
    constexpr RECT between = {506, 300, 1917, 700};
    CHECK(Same(ApplySnap(between, candidates, kDefaultSnapThresholdPx, Grip::Move),
               RECT{509, 300, 1920, 700}));
}

TEST_CASE("ApplySnap: a corner resize only attracts that corner's own edges") {
    const SnapCandidates candidates = WorkAreaOnly();
    constexpr RECT dragged = {5, 6, 900, 1035};
    const RECT snapped = ApplySnap(dragged, candidates, kDefaultSnapThresholdPx, Grip::TopLeft);
    // Top-left snapped to the work area; the anchored edges never moved.
    CHECK(Same(snapped, RECT{0, 0, 900, 1035}));

    const RECT bottomRight = ApplySnap(dragged, candidates, kDefaultSnapThresholdPx, Grip::BottomRight);
    CHECK(Same(bottomRight, RECT{5, 6, 900, 1040}));

    // An edge grip attracts that edge only -- the other axis is untouched
    // even where it sits well within the threshold of a candidate.
    const RECT leftOnly = ApplySnap(dragged, candidates, kDefaultSnapThresholdPx, Grip::Left);
    CHECK(Same(leftOnly, RECT{0, 6, 900, 1035}));
    const RECT bottomOnly = ApplySnap(dragged, candidates, kDefaultSnapThresholdPx, Grip::Bottom);
    CHECK(Same(bottomOnly, RECT{5, 6, 900, 1040}));
}

TEST_CASE("ClampToMonitor: a move is parked flush instead of spilling onto the next screen") {
    constexpr RECT pushedRight = {1700, 300, 2100, 700};
    CHECK(Same(ClampToMonitor(pushedRight, kMonitor, Grip::Move), RECT{1520, 300, 1920, 700}));

    constexpr RECT pushedLeft = {-80, 300, 320, 700};
    CHECK(Same(ClampToMonitor(pushedLeft, kMonitor, Grip::Move), RECT{0, 300, 400, 700}));
}

TEST_CASE("ClampToMonitor: a window wider than the monitor keeps its near edge on screen") {
    constexpr RECT oversized = {-200, -100, 2200, 1200};
    CHECK(Same(ClampToMonitor(oversized, kMonitor, Grip::Move), RECT{0, 0, 2400, 1300}));
}

TEST_CASE("ClampToMonitor: a resize clamps the dragged edges and leaves the anchored ones") {
    constexpr RECT dragged = {-50, -60, 900, 700};
    CHECK(Same(ClampToMonitor(dragged, kMonitor, Grip::TopLeft), RECT{0, 0, 900, 700}));

    constexpr RECT grown = {100, 200, 2000, 1500};
    CHECK(Same(ClampToMonitor(grown, kMonitor, Grip::BottomRight), RECT{100, 200, 1920, 1080}));
}

TEST_CASE("ShouldReleaseClamp: the boundary holds the clamp, past it releases") {
    CHECK_FALSE(ShouldReleaseClamp(0));
    CHECK_FALSE(ShouldReleaseClamp(kOvershootReleasePx));
    CHECK(ShouldReleaseClamp(kOvershootReleasePx + 1));
}

TEST_CASE("NextSnapInDirection: the leading edge lands flush on the next candidate") {
    const std::array<RECT, 1> others = {RECT{600, 200, 1000, 800}};
    const SnapCandidates candidates = CollectSnapEdges(kWorkArea, others);

    constexpr RECT window = {100, 300, 400, 600};
    // Moving right: the right edge (400) finds 600, so the window shifts 200.
    CHECK(Same(NextSnapInDirection(window, candidates, 1, 0), RECT{300, 300, 600, 600}));
    // Moving left: the left edge (100) finds 0.
    CHECK(Same(NextSnapInDirection(window, candidates, -1, 0), RECT{0, 300, 300, 600}));
    // Moving down: the bottom edge (600) finds 800.
    CHECK(Same(NextSnapInDirection(window, candidates, 0, 1), RECT{100, 500, 400, 800}));
}

TEST_CASE("NextSnapInDirection: with no candidate ahead, that axis does not move") {
    const SnapCandidates candidates = WorkAreaOnly();
    constexpr RECT flushRight = {1620, 300, 1920, 600};
    CHECK(Same(NextSnapInDirection(flushRight, candidates, 1, 0), flushRight));
    constexpr RECT flushLeft = {0, 300, 300, 600};
    CHECK(Same(NextSnapInDirection(flushLeft, candidates, -1, 0), flushLeft));
}

TEST_CASE("EnforceMinimumSize: the dragged edge is pushed back, the anchored edge stays") {
    constexpr RECT tiny = {500, 500, 510, 505};

    const RECT topLeft = EnforceMinimumSize(tiny, Grip::TopLeft, kMinWindowWidthPx, kMinWindowHeightPx);
    CHECK(Same(topLeft, RECT{510 - kMinWindowWidthPx, 505 - kMinWindowHeightPx, 510, 505}));

    const RECT bottomRight =
        EnforceMinimumSize(tiny, Grip::BottomRight, kMinWindowWidthPx, kMinWindowHeightPx);
    CHECK(Same(bottomRight, RECT{500, 500, 500 + kMinWindowWidthPx, 500 + kMinWindowHeightPx}));

    const RECT topRight = EnforceMinimumSize(tiny, Grip::TopRight, kMinWindowWidthPx, kMinWindowHeightPx);
    CHECK(Same(topRight, RECT{500, 505 - kMinWindowHeightPx, 500 + kMinWindowWidthPx, 505}));

    const RECT bottomLeft =
        EnforceMinimumSize(tiny, Grip::BottomLeft, kMinWindowWidthPx, kMinWindowHeightPx);
    CHECK(Same(bottomLeft, RECT{510 - kMinWindowWidthPx, 500, 510, 500 + kMinWindowHeightPx}));
}

TEST_CASE("EnforceMinimumSize: a window already big enough is untouched, and a move never resizes") {
    constexpr RECT roomy = {100, 100, 900, 700};
    CHECK(Same(EnforceMinimumSize(roomy, Grip::TopLeft, kMinWindowWidthPx, kMinWindowHeightPx), roomy));

    constexpr RECT tiny = {500, 500, 510, 505};
    CHECK(Same(EnforceMinimumSize(tiny, Grip::Move, kMinWindowWidthPx, kMinWindowHeightPx), tiny));
}

TEST_CASE("EnforceMinimumSize: an edge grip only corrects the axis it drags") {
    constexpr RECT tiny = {500, 500, 510, 505};

    // Top drags no horizontal edge, so the width is left as it found it
    // even though it is under the floor -- it cannot have caused that.
    const RECT top = EnforceMinimumSize(tiny, Grip::Top, kMinWindowWidthPx, kMinWindowHeightPx);
    CHECK(Same(top, RECT{500, 505 - kMinWindowHeightPx, 510, 505}));

    const RECT left = EnforceMinimumSize(tiny, Grip::Left, kMinWindowWidthPx, kMinWindowHeightPx);
    CHECK(Same(left, RECT{510 - kMinWindowWidthPx, 500, 510, 505}));
}

TEST_CASE("LayoutForPointer: edges offer halves and the top offers maximize") {
    constexpr RECT monitor = {0, 0, 1920, 1080};
    constexpr int edge = 6;
    constexpr int corner = 40;

    CHECK(LayoutForPointer({960, 2}, monitor, edge, corner) == SnapLayout::Maximize);
    CHECK(LayoutForPointer({2, 540}, monitor, edge, corner) == SnapLayout::LeftHalf);
    CHECK(LayoutForPointer({1917, 540}, monitor, edge, corner) == SnapLayout::RightHalf);
    CHECK(LayoutForPointer({960, 540}, monitor, edge, corner) == SnapLayout::None);
}

TEST_CASE("LayoutForPointer: the bottom edge deliberately offers nothing") {
    constexpr RECT monitor = {0, 0, 1920, 1080};
    // Natively it does not snap either, and claiming it would make
    // dragging near the taskbar unpredictable.
    CHECK(LayoutForPointer({960, 1079}, monitor, 6, 40) == SnapLayout::None);
}

TEST_CASE("LayoutForPointer: corners win over edges, or the quarter is unreachable") {
    constexpr RECT monitor = {0, 0, 1920, 1080};
    constexpr int edge = 6;
    constexpr int corner = 40;

    // Each of these is also inside an edge zone; the corner must win.
    CHECK(LayoutForPointer({2, 2}, monitor, edge, corner) == SnapLayout::TopLeftQuarter);
    CHECK(LayoutForPointer({1918, 2}, monitor, edge, corner) == SnapLayout::TopRightQuarter);
    CHECK(LayoutForPointer({2, 1078}, monitor, edge, corner) == SnapLayout::BottomLeftQuarter);
    CHECK(LayoutForPointer({1918, 1078}, monitor, edge, corner) == SnapLayout::BottomRightQuarter);

    // Within the corner square but well clear of any edge zone.
    CHECK(LayoutForPointer({30, 30}, monitor, edge, corner) == SnapLayout::TopLeftQuarter);
}

TEST_CASE("LayoutForPointer: a pointer that has left this monitor asks it for nothing") {
    constexpr RECT monitor = {0, 0, 1920, 1080};
    CHECK(LayoutForPointer({-1, 540}, monitor, 6, 40) == SnapLayout::None);
    CHECK(LayoutForPointer({1920, 540}, monitor, 6, 40) == SnapLayout::None);
}

TEST_CASE("LayoutForPointer: a monitor offset from the origin still works") {
    // The second monitor in a side-by-side pair -- the zones have to be
    // relative to its own rect, not to the desktop origin.
    constexpr RECT monitor = {1920, 0, 3840, 1080};
    CHECK(LayoutForPointer({1922, 540}, monitor, 6, 40) == SnapLayout::LeftHalf);
    CHECK(LayoutForPointer({3837, 540}, monitor, 6, 40) == SnapLayout::RightHalf);
    CHECK(LayoutForPointer({2880, 2}, monitor, 6, 40) == SnapLayout::Maximize);
}

TEST_CASE("RectForLayout: halves and quarters tile the work area exactly") {
    constexpr RECT work = {0, 0, 1921, 1081};  // odd, to catch a rounding gap

    const RECT left = RectForLayout(SnapLayout::LeftHalf, work);
    const RECT right = RectForLayout(SnapLayout::RightHalf, work);
    CHECK(left.right == right.left);            // no gap, no overlap
    CHECK(left.left == work.left);
    CHECK(right.right == work.right);
    CHECK(left.top == work.top);
    CHECK(left.bottom == work.bottom);

    const RECT tl = RectForLayout(SnapLayout::TopLeftQuarter, work);
    const RECT tr = RectForLayout(SnapLayout::TopRightQuarter, work);
    const RECT bl = RectForLayout(SnapLayout::BottomLeftQuarter, work);
    const RECT br = RectForLayout(SnapLayout::BottomRightQuarter, work);
    CHECK(tl.right == tr.left);
    CHECK(bl.right == br.left);
    CHECK(tl.bottom == bl.top);
    CHECK(tr.bottom == br.top);
    CHECK(tl.left == work.left);
    CHECK(br.right == work.right);
    CHECK(br.bottom == work.bottom);
}

TEST_CASE("RectForLayout: maximize is the whole work area, not the whole monitor") {
    constexpr RECT work = {0, 0, 1920, 1040};  // taskbar takes the last 40
    CHECK(Same(RectForLayout(SnapLayout::Maximize, work), work));
}

TEST_CASE("ZoneRect: the nine zones tile the window exactly, with no gap or overlap") {
    constexpr RECT window = {100, 100, 1100, 800};
    constexpr int b = 100;

    CHECK(Same(ZoneRect(Grip::TopLeft, window, b), RECT{100, 100, 200, 200}));
    CHECK(Same(ZoneRect(Grip::Top, window, b), RECT{200, 100, 1000, 200}));
    CHECK(Same(ZoneRect(Grip::TopRight, window, b), RECT{1000, 100, 1100, 200}));
    CHECK(Same(ZoneRect(Grip::Left, window, b), RECT{100, 200, 200, 700}));
    CHECK(Same(ZoneRect(Grip::Move, window, b), RECT{200, 200, 1000, 700}));
    CHECK(Same(ZoneRect(Grip::Right, window, b), RECT{1000, 200, 1100, 700}));
    CHECK(Same(ZoneRect(Grip::BottomLeft, window, b), RECT{100, 700, 200, 800}));
    CHECK(Same(ZoneRect(Grip::Bottom, window, b), RECT{200, 700, 1000, 800}));
    CHECK(Same(ZoneRect(Grip::BottomRight, window, b), RECT{1000, 700, 1100, 800}));
}

TEST_CASE("ZoneRect agrees with GripForPoint everywhere, so the map cannot lie") {
    // The map the UI draws and the hit-test that decides what a click
    // does have to be the same geometry. Walk the window and check that
    // every point lands inside the rect drawn for the zone it hit-tests
    // to.
    constexpr RECT window = {100, 100, 1100, 800};
    constexpr int b = 100;
    for (LONG y = window.top; y < window.bottom; y += 7) {
        for (LONG x = window.left; x < window.right; x += 7) {
            const Grip grip = GripForPoint(window, {x, y}, b);
            const RECT zone = ZoneRect(grip, window, b);
            CHECK(x >= zone.left);
            CHECK(x < zone.right);
            CHECK(y >= zone.top);
            CHECK(y < zone.bottom);
        }
    }
}

TEST_CASE("ZoneRect: no band means every zone is the whole window") {
    constexpr RECT window = {100, 100, 1100, 800};
    CHECK(Same(ZoneRect(Grip::Move, window, 0), window));
    CHECK(Same(ZoneRect(Grip::TopLeft, window, 0), window));
}
