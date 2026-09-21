#include "App.h"
#include "GpuProbe.h"
#include "ProofProbe.h"
#include "MiningSession.h"
#include "pearl_core.h"
#include <Windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <thread>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::UI::Core;

namespace {
void write_result(const JsonObject& result) {
    auto folder = ApplicationData::Current().LocalFolder();
    // Replace the last run in this probe's own LocalState only.
    auto file = folder.CreateFileAsync(L"pearl-probe-result.json",CreationCollisionOption::ReplaceExisting).get();
    FileIO::WriteTextAsync(file,result.Stringify()).get();
}

struct ProbeView : implements<ProbeView,IFrameworkView> {
    void Initialize(const CoreApplicationView& view) {
        view.Activated([](const auto&,const auto&) { CoreWindow::GetForCurrentThread().Activate(); });
    }
    void Load(const hstring&) {}
    void Uninitialize() {}
    void SetWindow(const CoreWindow&) {}
    void Run() {
        auto window = CoreWindow::GetForCurrentThread();
        window.Activate();
        auto dispatcher = window.Dispatcher();
        std::thread([dispatcher] {
            // WinRT must be initialized before constructing even a JsonObject.
            try {
                init_apartment(apartment_type::multi_threaded);
            } catch (...) {
                OutputDebugStringW(L"PearlProbe: worker WinRT initialization failed\n");
                dispatcher.RunAsync(CoreDispatcherPriority::Normal,[] { CoreApplication::Exit(); });
                return;
            }
            JsonObject result;
            try {
                result.Insert(L"status",JsonValue::CreateStringValue(L"running"));
                result.Insert(L"stage",JsonValue::CreateStringValue(L"core_reference_tests"));
                result.Insert(L"mining_enabled",JsonValue::CreateBooleanValue(false));
                result.Insert(L"shares_submitted",JsonValue::CreateNumberValue(0));
                result.Insert(L"scope",JsonValue::CreateStringValue(L"Complete dense V3 proof fixtures on GPU; offline test target only"));
                result.Insert(L"device_family",JsonValue::CreateStringValue(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily()));
                result.Insert(L"os_version",JsonValue::CreateStringValue(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamilyVersion()));
                write_result(result);
                result.Insert(L"core_checks",JsonValue::CreateNumberValue(pearl::self_test()));
                auto config_file=ApplicationData::Current().LocalFolder().TryGetItemAsync(L"pearl-mining-config.json").get();
                if(config_file) {
                    const auto config=JsonObject::Parse(FileIO::ReadTextAsync(config_file.as<StorageFile>()).get());
                    result.Insert(L"stage",JsonValue::CreateStringValue(L"live_pool_session"));
                    result.Insert(L"scope",JsonValue::CreateStringValue(L"Bounded live Pearl V3 session; revenue requires pool acknowledgement"));
                    result.Insert(L"mining_enabled",JsonValue::CreateBooleanValue(true));write_result(result);
                    const auto mining=run_mining_session(config);
                    result.Insert(L"mining",mining);
                    result.Insert(L"shares_submitted",mining.GetNamedValue(L"shares_submitted"));
                    result.Insert(L"status",JsonValue::CreateStringValue(mining.GetNamedString(L"stage")==L"completed_bounded_run"?L"completed":L"failed"));
                } else {
                    result.Insert(L"stage",JsonValue::CreateStringValue(L"complete_v3_gpu_proofs"));
                    write_result(result);
                    result.Insert(L"proofs",run_proof_probe());
                    result.Insert(L"status",JsonValue::CreateStringValue(L"passed"));
                }
            } catch (const hresult_error& e) {
                result.Insert(L"status",JsonValue::CreateStringValue(L"failed"));
                result.Insert(L"error",JsonValue::CreateStringValue(e.message()));
            } catch (const std::exception& e) {
                result.Insert(L"status",JsonValue::CreateStringValue(L"failed"));
                result.Insert(L"error",JsonValue::CreateStringValue(to_hstring(e.what())));
            }
            try { write_result(result); }
            catch (...) { OutputDebugStringW(L"PearlProbe: could not write LocalState result\n"); }
            dispatcher.RunAsync(CoreDispatcherPriority::Normal,[] { CoreApplication::Exit(); });
        }).detach();
        dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }
};
}

namespace winrt::PearlProbe::implementation {
IFrameworkView App::CreateView() { return make<ProbeView>(); }
}

int __stdcall wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        init_apartment(apartment_type::multi_threaded);
        CoreApplication::Run(make<winrt::PearlProbe::implementation::App>());
        return 0;
    } catch (const hresult_error& e) {
        OutputDebugStringW(e.message().c_str());
        return 1;
    }
}
