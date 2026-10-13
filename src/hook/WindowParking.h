#pragma once

#include "windowtracking/ScaledWindowSet.h"

#include <windows.h>

#include <optional>

namespace polish {

// Hiding another process's window while it keeps running -- the only code
// in Polish that changes a foreign window's extended styles.
//
// "Parked" means: invisible, click-through, still painting, still
// composed, so a DWM thumbnail of it stays live. That is WS_EX_LAYERED
// with alpha 1 plus WS_EX_TRANSPARENT, and optionally topmost so nothing
// can cover it (which is what Chromium's occlusion tracking would
// otherwise react to). The spike that chose this is recorded in PLAN.md:
// moving the window off-screen freezes Chromium, this does not.
//
// Because it changes someone else's window, every park is journalled
// before it is applied, and every way Polish can end puts the window
// back: a normal exit (UnparkAll by the caller), a crash (the unhandled-
// exception filter, which works from a fixed array and allocates
// nothing), and a hard kill (the journal, replayed at the next start).

// How hidden the parked window is kept.
enum class ParkStrategy {
    // Left in place, invisible and click-through.
    Ghost,
    // Ghost, plus topmost: nothing can ever be in front of it.
    GhostTopmost,
};

// Parks `hwnd`. Returns the record needed to put it back, or nullopt if it
// cannot be done safely: an elevated window (UIPI makes the style change a
// silent no-op), one that is already layered through UpdateLayeredWindow
// (its alpha is not ours to set), or one that is gone.
std::optional<ParkRecord> ParkWindow(HWND hwnd, ParkStrategy strategy);

// Puts `hwnd` back exactly as `record` says and drops its journal entry.
void UnparkWindow(HWND hwnd, const ParkRecord& record);

// True while the styles ParkWindow applied are still in place. Apps do
// reset their own styles now and then; the caller polls this and either
// re-parks or gives the window up.
bool IsStillParked(HWND hwnd, ParkStrategy strategy);

// Re-applies the park styles to a window whose app undid them. The record
// is unchanged: it still describes the pre-park state.
void ReapplyPark(HWND hwnd, ParkStrategy strategy);

// Startup: puts back every window a previous Polish run left parked and
// did not get to restore (killed, crashed hard, power cut). Matches on
// handle *and* process id, so a recycled handle is not touched. Call
// before anything else can park.
void RestoreParkedFromJournal();

// Installs the unhandled-exception filter that unparks everything
// currently parked. Idempotent.
void InstallParkingCrashHandler();

}  // namespace polish
