#pragma once
#include <winrt/Windows.Data.Json.h>
#include "pearl_core.h"
struct GpuWorkResult {
    std::vector<std::uint32_t> output;
    winrt::Windows::Data::Json::JsonObject measurements;
};
GpuWorkResult run_gpu_work(const pearl::Shape&, const pearl::Matrices&, bool compare_cpu);
winrt::Windows::Data::Json::JsonObject run_gpu_probe();
