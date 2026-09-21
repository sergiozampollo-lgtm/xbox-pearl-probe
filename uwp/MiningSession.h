#pragma once
#include <winrt/Windows.Data.Json.h>
// Called only when the owner has supplied a private LocalState configuration.
winrt::Windows::Data::Json::JsonObject run_mining_session(
    const winrt::Windows::Data::Json::JsonObject& config);
