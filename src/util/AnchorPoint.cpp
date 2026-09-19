#include "util/AnchorPoint.h"

#include "windowtracking/RectUtils.h"

namespace polish {

Anchor ResolveInteractionAnchor() {
    HWND foreground = GetForegroundWindow();
    RECT windowRect;
    if (foreground == nullptr || !GetVisibleWindowRect(foreground, windowRect)) {
        return {};
    }

    // rcCaret is in hwndCaret's client coordinates. Its width is often 0
    // or 1 (a thin bar), so "non-empty" means it has height.
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const DWORD threadId = GetWindowThreadProcessId(foreground, nullptr);
    if (threadId != 0 && GetGUIThreadInfo(threadId, &info) && info.hwndCaret != nullptr &&
        info.rcCaret.bottom > info.rcCaret.top) {
        POINT topLeft{info.rcCaret.left, info.rcCaret.top};
        POINT bottomRight{info.rcCaret.right, info.rcCaret.bottom};
        if (ClientToScreen(info.hwndCaret, &topLeft) && ClientToScreen(info.hwndCaret, &bottomRight)) {
            const POINT caret{(topLeft.x + bottomRight.x) / 2, (topLeft.y + bottomRight.y) / 2};
            if (PtInRect(&windowRect, caret)) {
                return {caret, AnchorSource::Caret};
            }
        }
    }

    POINT cursor;
    if (GetCursorPos(&cursor) && PtInRect(&windowRect, cursor)) {
        return {cursor, AnchorSource::Cursor};
    }

    return {POINT{(windowRect.left + windowRect.right) / 2, (windowRect.top + windowRect.bottom) / 2},
            AnchorSource::WindowCenter};
}

}  // namespace polish
