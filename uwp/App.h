#pragma once
#include "App.g.h"
namespace winrt::PearlProbe::implementation {
struct App : AppT<App> {
    App() = default;
    Windows::ApplicationModel::Core::IFrameworkView CreateView();
};
}
namespace winrt::PearlProbe::factory_implementation {
struct App : AppT<App, implementation::App> {};
}
