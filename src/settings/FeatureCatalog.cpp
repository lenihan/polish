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
        {Feature::ScaledTopmost, L"Scaled windows stay on top",
         L"A window you shrank to keep an eye on is no use once something covers it, so its thumbnail floats "
         L"above other windows. Turn this off to let them be covered like any other window.",
         &Settings::scaledViewTopmost},
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

const std::vector<NumberInfo>& AllNumbers() {
    static const std::vector<NumberInfo> kNumbers = {
        {Number::SnapDistance, L"Snap distance", L"pixels", 0, 60, &Settings::moveModeSnapThresholdPx},
        // 10..90 is what LoadSettings accepts; anything else it resets to 50.
        {Number::ScalePercent, L"Scaled window size", L"percent", 10, 90, &Settings::scaleDefaultPercent},
    };
    return kNumbers;
}

const NumberInfo& NumberFor(Number id) {
    for (const NumberInfo& info : AllNumbers()) {
        if (info.id == id) {
            return info;
        }
    }
    return AllNumbers().front();  // see FeatureFor
}

UINT GetNumber(const Settings& settings, Number id) { return settings.*(NumberFor(id).member); }

void SetNumber(Settings& settings, Number id, UINT value) {
    const NumberInfo& info = NumberFor(id);
    settings.*(info.member) = value < info.min ? info.min : (value > info.max ? info.max : value);
}

const std::vector<HotkeyInfo>& AllHotkeys() {
    static const std::vector<HotkeyInfo> kHotkeys = {
        {Hotkey::NewStack, L"New stack", &Settings::stackHotkeyModifiers, &Settings::stackHotkeyVirtualKey},
        {Hotkey::TileTwoWay, L"Tile 2 windows", &Settings::arrangeTwoWayHotkeyModifiers,
         &Settings::arrangeTwoWayHotkeyVirtualKey},
        {Hotkey::TileThreeWay, L"Tile 3 windows", &Settings::arrangeThreeWayHotkeyModifiers,
         &Settings::arrangeThreeWayHotkeyVirtualKey},
        {Hotkey::TileFourWay, L"Tile 4 windows", &Settings::arrangeFourWayHotkeyModifiers,
         &Settings::arrangeFourWayHotkeyVirtualKey},
        {Hotkey::LayoutMenu, L"Layout menu", &Settings::layoutHotkeyModifiers, &Settings::layoutHotkeyVirtualKey},
        {Hotkey::ScaleDown, L"Scale window down", &Settings::scaleDownHotkeyModifiers,
         &Settings::scaleDownHotkeyVirtualKey},
        {Hotkey::ScaleUp, L"Scale window up", &Settings::scaleUpHotkeyModifiers, &Settings::scaleUpHotkeyVirtualKey},
        {Hotkey::ScaleReset, L"Restore window to full size", &Settings::scaleResetHotkeyModifiers,
         &Settings::scaleResetHotkeyVirtualKey},
    };
    return kHotkeys;
}

const HotkeyInfo& HotkeyFor(Hotkey id) {
    for (const HotkeyInfo& info : AllHotkeys()) {
        if (info.id == id) {
            return info;
        }
    }
    return AllHotkeys().front();  // see FeatureFor
}

HotkeyCombination GetHotkey(const Settings& settings, Hotkey id) {
    const HotkeyInfo& info = HotkeyFor(id);
    return {settings.*(info.modifiers), settings.*(info.virtualKey)};
}

void SetHotkey(Settings& settings, Hotkey id, HotkeyCombination combination) {
    const HotkeyInfo& info = HotkeyFor(id);
    settings.*(info.modifiers) = combination.modifiers;
    settings.*(info.virtualKey) = combination.virtualKey;
}

}  // namespace polish
