#include "windowtracking/TaskbarButtons.h"

#include <algorithm>

namespace polish {

std::optional<TaskbarButton> HitTestTaskbarButton(const std::vector<TaskbarButton>& buttons, POINT screenPoint) {
    for (const TaskbarButton& button : buttons) {
        // PtInRect is exclusive on right/bottom, which is what we want:
        // adjacent buttons share an edge, and an inclusive test would make
        // that one column of pixels belong to both.
        if (PtInRect(&button.rect, screenPoint)) {
            return button;
        }
    }
    return std::nullopt;
}

bool TaskbarButtonsEqual(const std::vector<TaskbarButton>& a, const std::vector<TaskbarButton>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](const TaskbarButton& l, const TaskbarButton& r) {
        return l.appId == r.appId && l.name == r.name && l.taskbar == r.taskbar && l.rect.left == r.rect.left &&
               l.rect.top == r.rect.top && l.rect.right == r.rect.right && l.rect.bottom == r.rect.bottom;
    });
}

}  // namespace polish
