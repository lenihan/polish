#include "windowtracking/WindowZOrder.h"

namespace polish {

bool PromoteWindowToFront(HWND hwnd) {
    const BOOL promoted = SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!promoted) {
        return false;
    }
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return true;
}

}  // namespace polish
