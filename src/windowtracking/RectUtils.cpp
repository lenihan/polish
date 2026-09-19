#include "windowtracking/RectUtils.h"

#include <dwmapi.h>

#include <cstdlib>

namespace polish {

bool RectsApproximatelyEqual(const RECT& a, const RECT& b, int epsilonPixels) {
    return std::abs(a.left - b.left) <= epsilonPixels && std::abs(a.top - b.top) <= epsilonPixels &&
           std::abs(a.right - b.right) <= epsilonPixels && std::abs(a.bottom - b.bottom) <= epsilonPixels;
}

bool GetVisibleWindowRect(HWND hwnd, RECT& rect) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        return true;
    }
    return GetWindowRect(hwnd, &rect) != FALSE;
}

}  // namespace polish
