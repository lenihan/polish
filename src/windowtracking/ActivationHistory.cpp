#include "windowtracking/ActivationHistory.h"

#include <algorithm>

namespace polish {

void ActivationHistory::MoveToFront(HWND hwnd) {
    auto it = std::find(order_.begin(), order_.end(), hwnd);
    if (it != order_.end()) {
        order_.erase(it);
    }
    order_.insert(order_.begin(), hwnd);
}

void ActivationHistory::Remove(HWND hwnd) {
    order_.erase(std::remove(order_.begin(), order_.end(), hwnd), order_.end());
}

std::vector<HWND> ActivationHistory::OrderedWindows() const { return order_; }

}  // namespace polish
