#include "util/SwitcherCycle.h"

namespace polish {

size_t AdvanceHighlight(size_t base, size_t count, bool backward) {
    if (count == 0) {
        return 0;
    }
    return backward ? (base + count - 1) % count : (base + 1) % count;
}

}  // namespace polish
