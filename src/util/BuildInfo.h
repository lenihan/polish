#pragma once

#include <string>

namespace polish {

// Which build of Polish this is -- not just which version.
//
// The version alone cannot answer the question that actually comes up
// while developing: two binaries both say 0.3.0 and behave differently,
// and there is nothing on screen to tell you which one is running. So the
// commit, whether the tree had uncommitted changes in it, and the build
// time are baked in alongside it, and the tray icon's tooltip shows the
// lot (see TrayIcon's `tooltip`).
//
// The definitions live in a generated BuildInfo.cpp, re-stamped before
// every build by cmake/BuildStamp.cmake -- a stamp written only at
// configure time would go stale on the next rebuild, which is exactly
// the case it is for. Only the executables link it, never polish_core,
// so re-stamping costs one small translation unit and a relink and never
// disturbs the test binary.

// "0.3.0" -- from project(polish VERSION ...), the same single source
// the VERSIONINFO resource is generated from.
const wchar_t* BuildVersion();

// The short commit this was built from, or "unknown" outside a git
// checkout (a source archive, say) or with no git on PATH. Never empty,
// so callers never have to special-case it for display.
const wchar_t* BuildCommit();

// Whether tracked files had uncommitted changes at build time. True means
// the commit above does not fully describe what is in the binary.
// Untracked files deliberately do not count -- see BuildStamp.cmake.
bool BuildIsModified();

// Local wall-clock build time, "YYYY-MM-DD HH:MM". The discriminator of
// last resort, and the only one that separates two builds of the same
// dirty tree.
const wchar_t* BuildTime();

// The whole thing on one line, ready to show:
//   "0.3.0 - 9c488c8+ - 2026-10-03 14:22"
// The "+" marks a modified tree, the way git describe and most prompts
// mark one, so it reads at a glance without taking up a word.
std::wstring BuildStamp();

}  // namespace polish
