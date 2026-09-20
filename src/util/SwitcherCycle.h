#pragma once

#include <cstddef>

namespace polish {

// The pure arithmetic shared by both switchers -- Alt+Tab over windows
// and Alt+` over the foreground window's tabs. They cycle identically
// over different things, so this lives apart from either.

// The highlight index after one Tab (or Shift+Tab, when `backward`) from
// `base` in a list of `count` entries, wrapping at both ends.
//
// Returns 0 for an empty list rather than dividing by zero, and for a
// single-entry list (where every step lands back on the only entry).
size_t AdvanceHighlight(size_t base, size_t count, bool backward);

}  // namespace polish
