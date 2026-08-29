#include "windowtracking/RectUtils.h"

#include <cstdlib>

namespace polish {

bool RectsApproximatelyEqual(const RECT& a, const RECT& b, int epsilonPixels) {
    return std::abs(a.left - b.left) <= epsilonPixels && std::abs(a.top - b.top) <= epsilonPixels &&
           std::abs(a.right - b.right) <= epsilonPixels && std::abs(a.bottom - b.bottom) <= epsilonPixels;
}

}  // namespace polish
