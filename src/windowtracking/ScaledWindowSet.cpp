#include "windowtracking/ScaledWindowSet.h"

#include "windowtracking/ScaleGeometry.h"

#include <algorithm>

namespace polish {

ScaledEntry* ScaledWindowSet::FindMutable(HWND hwnd) {
    for (ScaledEntry& entry : entries_) {
        if (entry.hwnd == hwnd) {
            return &entry;
        }
    }
    return nullptr;
}

const ScaledEntry* ScaledWindowSet::Find(HWND hwnd) const {
    for (const ScaledEntry& entry : entries_) {
        if (entry.hwnd == hwnd) {
            return &entry;
        }
    }
    return nullptr;
}

bool ScaledWindowSet::Contains(HWND hwnd) const {
    return Find(hwnd) != nullptr;
}

void ScaledWindowSet::Touch(HWND hwnd) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ScaledEntry& e) { return e.hwnd == hwnd; });
    if (it != entries_.end() && it + 1 != entries_.end()) {
        std::rotate(it, it + 1, entries_.end());
    }
}

bool ScaledWindowSet::Scale(const ScaledEntry& entry) {
    if (entry.hwnd == nullptr || Contains(entry.hwnd)) {
        return false;
    }
    entries_.push_back(entry);
    // Scaling it again starts from where this scaling put it, not from a
    // stale position remembered from an earlier round.
    std::erase_if(remembered_, [&](const RememberedSlot& slot) { return slot.hwnd == entry.hwnd; });
    return true;
}

std::optional<ScaledEntry> ScaledWindowSet::Unscale(HWND hwnd) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ScaledEntry& e) { return e.hwnd == hwnd; });
    if (it == entries_.end()) {
        return std::nullopt;
    }
    const ScaledEntry removed = *it;
    entries_.erase(it);
    std::erase_if(remembered_, [&](const RememberedSlot& slot) { return slot.hwnd == hwnd; });
    remembered_.push_back({hwnd, {removed.viewRect, removed.scale}});
    return removed;
}

bool ScaledWindowSet::SetViewRect(HWND hwnd, const RECT& viewRect) {
    ScaledEntry* entry = FindMutable(hwnd);
    if (entry == nullptr) {
        return false;
    }
    entry->viewRect = viewRect;
    entry->scale = ScaleOf(viewRect, entry->realSize);
    Touch(hwnd);
    return true;
}

bool ScaledWindowSet::Rescale(HWND hwnd, double scale) {
    ScaledEntry* entry = FindMutable(hwnd);
    if (entry == nullptr) {
        return false;
    }
    scale = std::clamp(scale, kMinScale, kMaxScale);
    entry->scale = scale;
    entry->viewRect = ScaledViewRect(entry->realSize, scale, {entry->viewRect.left, entry->viewRect.top});
    Touch(hwnd);
    return true;
}

bool ScaledWindowSet::OnRealResized(HWND hwnd, SIZE newRealSize) {
    ScaledEntry* entry = FindMutable(hwnd);
    if (entry == nullptr || newRealSize.cx <= 0 || newRealSize.cy <= 0) {
        return false;
    }
    entry->realSize = newRealSize;
    entry->viewRect = ResizeViewKeepingAnchor(entry->viewRect, entry->scale, newRealSize);
    return true;
}

bool ScaledWindowSet::SetMinimized(HWND hwnd, bool minimized) {
    ScaledEntry* entry = FindMutable(hwnd);
    if (entry == nullptr) {
        return false;
    }
    entry->minimized = minimized;
    return true;
}

void ScaledWindowSet::OnRealDestroyed(HWND hwnd) {
    std::erase_if(entries_, [&](const ScaledEntry& e) { return e.hwnd == hwnd; });
    std::erase_if(remembered_, [&](const RememberedSlot& slot) { return slot.hwnd == hwnd; });
}

HWND ScaledWindowSet::MostRecent() const {
    return entries_.empty() ? nullptr : entries_.back().hwnd;
}

std::optional<RememberedView> ScaledWindowSet::Remembered(HWND hwnd) const {
    for (const RememberedSlot& slot : remembered_) {
        if (slot.hwnd == hwnd) {
            return slot.view;
        }
    }
    return std::nullopt;
}

}  // namespace polish
