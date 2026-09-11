#include "util/WindowIcon.h"

namespace polish {

HICON GetWindowIconHandle(HWND hwnd) {
    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    return icon;
}

}  // namespace polish
