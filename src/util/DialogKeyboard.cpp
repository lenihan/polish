#include "util/DialogKeyboard.h"

namespace polish {

void CycleFocus(const HWND* stops, size_t count, bool backward) {
    if (stops == nullptr || count == 0) {
        return;
    }

    const HWND focused = GetFocus();
    size_t index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (stops[i] != nullptr && stops[i] == focused) {
            index = i;
            break;
        }
    }

    for (size_t step = 0; step < count; ++step) {
        index = backward ? (index + count - 1) % count : (index + 1) % count;
        if (stops[index] != nullptr && IsWindowEnabled(stops[index])) {
            SetFocus(stops[index]);
            return;
        }
    }
}

bool IsDialogKeyDown(const MSG& msg, HWND dialog, UINT virtualKey) {
    return msg.message == WM_KEYDOWN && msg.wParam == virtualKey && dialog != nullptr &&
           (msg.hwnd == dialog || IsChild(dialog, msg.hwnd) != FALSE);
}

}  // namespace polish
