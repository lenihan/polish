#pragma once

#include <windows.h>

namespace polish {

// The real font Windows itself uses for UI body text at `dpi`
// (NONCLIENTMETRICS' lfMessageFont, queried per-DPI via
// SystemParametersInfoForDpi) -- *not* GetStockObject(DEFAULT_GUI_FONT),
// which is a fixed, pre-DPI-awareness bitmap font that leaves text tiny
// inside otherwise-correctly-scaled UI at high DPI. That has now been
// confirmed, human-reported, twice: once in the picker's own lists, and
// again in the stack's tab strip, which was still on the stock
// font after every *other* tab-strip dimension had already been
// DPI-scaled.
//
// Every caller that draws its own text should use this rather than
// re-deriving it -- there were six verbatim copies of the same four
// lines across the picker, its two list panels, the Alt+Tab list and the
// stack strip before this existed.
//
// The caller owns the returned HFONT and must DeleteObject it (unlike a
// stock font, which must *not* be deleted -- a trap when converting a
// GetStockObject call site over to this one).
HFONT MakeUiFont(UINT dpi);

// MakeUiFont's weight-overridden sibling, for a call site that needs a
// matching bold (or otherwise re-weighted) variant of the same face --
// e.g. an active tab's label, or a list's section heading. Same
// ownership rule: the caller DeleteObject's it.
HFONT MakeUiFontWithWeight(UINT dpi, LONG weight);

}  // namespace polish
