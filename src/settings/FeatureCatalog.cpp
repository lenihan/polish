#include "settings/FeatureCatalog.h"

namespace polish {

const std::vector<FeatureInfo>& AllFeatures() {
    static const std::vector<FeatureInfo> kFeatures = {
        {Feature::AltTab, L"Alt+Tab and Alt+` switchers",
         L"Replaces Alt+Tab with a list of your windows you can drive from the keyboard or the mouse -- close, "
         L"minimize or maximize a window right from its row -- and adds Alt+` to switch between the tabs of the "
         L"window in front.",
         &Settings::altTabEnabled},
        {Feature::Taskbar, L"Taskbar hover list and click-to-cycle",
         L"Hover a taskbar button for a list of that app's windows, with close and minimize buttons, instead of "
         L"Windows' thumbnails. Clicking an app's button cycles through its windows.",
         &Settings::taskbarEnabled},
        {Feature::Halo, L"Halo around active window",
         L"A soft glow in your accent colour around the window you are working in, so there is never any doubt "
         L"which one has the keyboard.",
         &Settings::haloEnabled},
        {Feature::Bullseye, L"Bullseye (copy/paste flash)",
         L"A ring that flashes where you copy or paste, so you can see it happened. A copy that put nothing on "
         L"the clipboard shows a red X instead.",
         &Settings::bullseyeEnabled},
        {Feature::KeepClipboard, L"Keep the clipboard on an empty copy",
         L"Some editors copy an empty line when you press Ctrl+C with nothing selected, which quietly replaces "
         L"whatever you had copied. Polish puts your clipboard back and shows the red X.",
         &Settings::keepClipboardOnEmptyCopy},
        {Feature::MoveMode, L"Hold Win to move/resize any window",
         L"Hold the Windows key and drag anywhere on a window to move it, or near an edge or corner to resize "
         L"it. Drop at the edge of the screen to snap it to a half, a quarter or full screen.",
         &Settings::moveModeEnabled},
        {Feature::Scale, L"Scaled windows (live thumbnail)",
         L"Shrink a window to a small live thumbnail you can keep an eye on, then click it to bring the window "
         L"back at full size. Nothing is resized -- the window is simply hidden behind its own thumbnail.",
         &Settings::scaleEnabled},
        {Feature::RestoreSync, L"Restore remembers Snap position",
         L"When you maximize a window you had snapped or moved, Windows can restore it to a stale spot. Polish "
         L"keeps its idea of the restored position in step with where you actually left the window.",
         &Settings::restoreSyncEnabled},
        {Feature::PolishButton, L"Polish button on taskbar",
         L"A button beside Start that opens Polish's window list and layout actions, so everything is one "
         L"click away without remembering a shortcut.",
         &Settings::polishButtonEnabled},
    };
    return kFeatures;
}

const FeatureInfo& FeatureFor(Feature id) {
    for (const FeatureInfo& info : AllFeatures()) {
        if (info.id == id) {
            return info;
        }
    }
    // Unreachable while the table covers every enumerator -- the test
    // that walks the enum is what keeps it that way. The first entry is a
    // harmless answer rather than a crash.
    return AllFeatures().front();
}

bool IsFeatureEnabled(const Settings& settings, Feature id) {
    return settings.*(FeatureFor(id).member);
}

void SetFeatureEnabled(Settings& settings, Feature id, bool enabled) {
    settings.*(FeatureFor(id).member) = enabled;
}

}  // namespace polish
