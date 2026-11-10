#pragma once

namespace polish {

// Whether this process is running from an MSIX package. A packaged app cannot
// use the Run registry key to start with Windows (its HKCU writes are
// redirected into the package), so it uses the startup task declared in the
// manifest instead.
bool IsPackaged();

// The manifest's startup task. Only meaningful when IsPackaged().
bool IsPackagedStartupEnabled();
void SetPackagedStartupEnabled(bool enabled);

}  // namespace polish
