#include "windowtracking/RectUtils.h"

#include <doctest/doctest.h>

using namespace polish;

TEST_CASE("RectsApproximatelyEqual: identical rects are equal") {
    constexpr RECT r = {0, 0, 100, 100};
    CHECK(RectsApproximatelyEqual(r, r));
}

TEST_CASE("RectsApproximatelyEqual: within epsilon on every edge is equal") {
    constexpr RECT a = {100, 100, 500, 500};
    constexpr RECT b = {101, 99, 501, 499};  // off by 1px on each edge
    CHECK(RectsApproximatelyEqual(a, b, /*epsilonPixels=*/2));
}

TEST_CASE("RectsApproximatelyEqual: beyond epsilon on any single edge is not equal") {
    constexpr RECT a = {100, 100, 500, 500};
    constexpr RECT rightOff = {100, 100, 510, 500};
    CHECK_FALSE(RectsApproximatelyEqual(a, rightOff, /*epsilonPixels=*/2));
}
