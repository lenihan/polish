#include "settings/PackagedStartup.h"

#include <windows.h>

#include <appmodel.h>

#include <thread>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

namespace polish {

namespace {

using winrt::Windows::ApplicationModel::StartupTask;
using winrt::Windows::ApplicationModel::StartupTaskState;

// The id the manifest gives the task (msix/AppxManifest.xml).
constexpr wchar_t kTaskId[] = L"PolishStartup";

// WinRT's blocking waits are not allowed on a UI thread's apartment, so each
// call runs on a short-lived thread of its own.
template <typename Fn>
auto OnWorkerThread(Fn fn) {
    decltype(fn()) result{};
    std::thread([&] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            result = fn();
        } catch (...) {
        }
    }).join();
    return result;
}

}  // namespace

bool IsPackaged() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

bool IsPackagedStartupEnabled() {
    return OnWorkerThread([] {
        const StartupTask task = StartupTask::GetAsync(kTaskId).get();
        const StartupTaskState state = task.State();
        return state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy;
    });
}

void SetPackagedStartupEnabled(bool enabled) {
    OnWorkerThread([enabled] {
        const StartupTask task = StartupTask::GetAsync(kTaskId).get();
        if (enabled) {
            // May be refused: the user can turn the task off in Settings > Apps >
            // Startup, and from then on only they can turn it back on.
            task.RequestEnableAsync().get();
        } else {
            task.Disable();
        }
        return true;
    });
}

}  // namespace polish
