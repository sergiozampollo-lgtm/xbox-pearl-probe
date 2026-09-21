#pragma once
#include <winrt/Windows.Data.Json.h>
#include "pearl_core.h"
#include <functional>
struct GpuWorkResult {
    std::vector<std::uint32_t> output;
    winrt::Windows::Data::Json::JsonObject measurements;
};
GpuWorkResult run_gpu_work(const pearl::Shape&, const pearl::Matrices&, bool compare_cpu);
void select_gpu_kernel(const std::string& name);
winrt::Windows::Data::Json::JsonObject run_gpu_variant_probe(const std::function<void(const std::string&)>& progress);
winrt::Windows::Data::Json::JsonObject run_gpu_probe();
