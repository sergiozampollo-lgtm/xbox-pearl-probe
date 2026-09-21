#include "GpuProbe.h"
#include "pearl_core.h"
#include <Windows.h>
#include "matmul_shader.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Data::Json;
namespace {
void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream s;
        s << operation << " HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(s.str());
    }
}

struct Event {
    HANDLE handle = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    Event() { if (!handle) throw std::runtime_error("CreateEventExW failed"); }
    ~Event() { if (handle) CloseHandle(handle); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};

// System runtime only. No Agility SDK, CUDA, software adapter, or CPU fallback.
struct Gpu {
    HMODULE runtime = nullptr; // deliberately kept loaded for the process life
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    Event event;
    UINT64 fence_value = 0;
    UINT64 timestamp_frequency = 0;
    struct Buffers {
        std::size_t a_bytes=0,b_bytes=0,out_bytes=0;
        bool used=false;
        ComPtr<ID3D12Resource> a_upload,b_upload,a,b,out,readback,timing;
        ComPtr<ID3D12QueryHeap> queries;
    };
    std::unique_ptr<Buffers> cached;

    Gpu() {
        runtime = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!runtime) throw std::runtime_error("system d3d12.dll unavailable");
        const auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(runtime,"D3D12CreateDevice"));
        const auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(runtime,"D3D12SerializeRootSignature"));
        if (!create || !serialize) throw std::runtime_error("system D3D12 exports unavailable");
        ComPtr<IDXGIFactory4> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapters1(i, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            checked(hr, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 description{};
            checked(candidate->GetDesc1(&description), "GetDesc1");
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(create(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                adapter = candidate;
                break;
            }
        }
        if (!device) throw std::runtime_error("No hardware D3D12 adapter; refusing CPU fallback");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        checked(queue->GetTimestampFrequency(&timestamp_frequency), "GetTimestampFrequency");
        if (!timestamp_frequency) throw std::runtime_error("zero GPU timestamp frequency");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "CreateCommandList");
        checked(commands->Close(), "initial Close");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)), "CreateFence");

        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 4;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor.ShaderRegister = 0;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[2].Descriptor.ShaderRegister = 1;
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[3].Descriptor.ShaderRegister = 0;
        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 4;
        root_desc.pParameters = parameters;
        ComPtr<ID3DBlob> blob, errors;
        checked(serialize(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors), "SerializeRootSignature");
        checked(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)), "CreateRootSignature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.CS = {g_matmul_shader, sizeof(g_matmul_shader)};
        checked(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline)), "CreateComputePipelineState");
    }

    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE heap,
                                D3D12_RESOURCE_STATES state, bool uav=false) {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = heap;
        props.CreationNodeMask = props.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (uav) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> out;
        checked(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out)), "CreateCommittedResource");
        return out;
    }
    void begin() {
        checked(allocator->Reset(), "allocator Reset");
        checked(commands->Reset(allocator.Get(),pipeline.Get()), "command Reset");
    }
    void flush() {
        checked(commands->Close(), "command Close");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1,lists);
        checked(queue->Signal(fence.Get(),++fence_value), "Signal");
        checked(fence->SetEventOnCompletion(fence_value,event.handle), "SetEventOnCompletion");
        if (WaitForSingleObjectEx(event.handle,15000,FALSE) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU fence timeout after 15 seconds");
        checked(device->GetDeviceRemovedReason(), "device status");
    }
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
        commands->ResourceBarrier(1,&barrier);
    }
    void upload(ID3D12Resource* resource, const void* data, std::size_t bytes) {
        void* mapped = nullptr;
        D3D12_RANGE empty{0,0};
        checked(resource->Map(0,&empty,&mapped), "upload Map");
        std::memcpy(mapped,data,bytes);
        D3D12_RANGE written{0,bytes};
        resource->Unmap(0,&written);
    }

    JsonObject run(const pearl::Shape& shape, const pearl::Matrices& input,
                   std::vector<std::uint32_t>& output, bool compare_cpu, unsigned repeat=1) {
        shape.validate_probe();
        if(input.a.size()!=std::size_t(shape.m)*shape.k || input.bt.size()!=std::size_t(shape.n)*shape.k)
            throw std::invalid_argument("GPU input size mismatch");
        const auto expected = compare_cpu ? pearl::reference_matmul(shape,input) : std::vector<std::uint32_t>{};
        const std::size_t output_bytes = shape.output_words()*sizeof(std::uint32_t);
        if(!cached || cached->a_bytes!=input.a.size() || cached->b_bytes!=input.bt.size() || cached->out_bytes!=output_bytes) {
            // All prior work has completed before replacing a shape's resources.
            cached=std::make_unique<Buffers>();
            cached->a_bytes=input.a.size();cached->b_bytes=input.bt.size();cached->out_bytes=output_bytes;
            cached->a_upload=buffer(input.a.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            cached->b_upload=buffer(input.bt.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            cached->a=buffer(input.a.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            cached->b=buffer(input.bt.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            cached->out=buffer(output_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
            cached->readback=buffer(output_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            cached->timing=buffer(2*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_QUERY_HEAP_DESC query_desc{};query_desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query_desc.Count=2;
            checked(device->CreateQueryHeap(&query_desc,IID_PPV_ARGS(&cached->queries)), "CreateQueryHeap");
        }
        const auto &a_upload=cached->a_upload,&b_upload=cached->b_upload,&a=cached->a,&b=cached->b;
        const auto &out=cached->out,&readback=cached->readback,&timing=cached->timing;
        const auto& queries=cached->queries;
        upload(a_upload.Get(),input.a.data(),input.a.size());
        upload(b_upload.Get(),input.bt.data(),input.bt.size());
        begin();
        if(cached->used) {
            transition(a.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            transition(b.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        }
        commands->CopyBufferRegion(a.Get(),0,a_upload.Get(),0,input.a.size());
        commands->CopyBufferRegion(b.Get(),0,b_upload.Get(),0,input.bt.size());
        transition(a.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(b.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        flush();

        JsonArray iterations;
        for (unsigned iteration=0; iteration<repeat; ++iteration) {
            begin();
            if (iteration || cached->used) transition(out.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetComputeRootSignature(root.Get());
            const UINT constants[] = {shape.m,shape.n,shape.k,shape.rank};
            commands->SetComputeRoot32BitConstants(0,4,constants,0);
            commands->SetComputeRootShaderResourceView(1,a->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(2,b->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(3,out->GetGPUVirtualAddress());
            commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            commands->Dispatch(shape.n/16,shape.m/16,1);
            commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
            transition(out.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(readback.Get(),0,out.Get(),0,output_bytes);
            commands->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timing.Get(),0);
            const auto start = std::chrono::steady_clock::now();
            flush();
            const auto finish = std::chrono::steady_clock::now();

            void* mapped = nullptr;
            D3D12_RANGE range{0,output_bytes}, no_write{0,0};
            checked(readback->Map(0,&range,&mapped), "result Map");
            const auto* actual = static_cast<const std::uint32_t*>(mapped);
            output.assign(actual,actual+shape.output_words());
            const auto digest = pearl::digest(mapped,output_bytes);
            readback->Unmap(0,&no_write);
            if (compare_cpu && output != expected)
                throw std::runtime_error("GPU/CPU output mismatch");
            D3D12_RANGE time_range{0,2*sizeof(UINT64)};
            checked(timing->Map(0,&time_range,&mapped), "timestamp Map");
            UINT64 ticks[2];
            std::memcpy(ticks,mapped,sizeof(ticks));
            timing->Unmap(0,&no_write);
            if (ticks[1] <= ticks[0]) throw std::runtime_error("non-increasing GPU timestamps");
            const double gpu_ms = 1000.0*static_cast<double>(ticks[1]-ticks[0])/static_cast<double>(timestamp_frequency);
            JsonObject record;
            record.Insert(L"warmup",JsonValue::CreateBooleanValue(iteration==0));
            record.Insert(L"gpu_ms",JsonValue::CreateNumberValue(gpu_ms));
            record.Insert(L"submit_wait_ms",JsonValue::CreateNumberValue(std::chrono::duration<double,std::milli>(finish-start).count()));
            record.Insert(L"cpu_comparison_performed",JsonValue::CreateBooleanValue(compare_cpu));
            if(compare_cpu) record.Insert(L"full_output_match",JsonValue::CreateBooleanValue(true));
            record.Insert(L"output_blake3",JsonValue::CreateStringValue(winrt::to_hstring(pearl::to_hex(digest))));
            iterations.Append(record);
        }
        JsonObject result;
        cached->used=true;
        result.Insert(L"m",JsonValue::CreateNumberValue(shape.m));
        result.Insert(L"n",JsonValue::CreateNumberValue(shape.n));
        result.Insert(L"k",JsonValue::CreateNumberValue(shape.k));
        result.Insert(L"rank",JsonValue::CreateNumberValue(shape.rank));
        result.Insert(L"checked_words_per_iteration",JsonValue::CreateNumberValue(static_cast<double>(expected.size())));
        result.Insert(L"iterations",iterations);
        return result;
    }
};
}

JsonObject run_gpu_probe() {
    Gpu gpu;
    JsonObject result;
    DXGI_ADAPTER_DESC1 description{};
    checked(gpu.adapter->GetDesc1(&description), "adapter description");
    result.Insert(L"adapter",JsonValue::CreateStringValue(description.Description));
    result.Insert(L"software_adapter",JsonValue::CreateBooleanValue(false));
    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(gpu.adapter.As(&adapter3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory)))
            result.Insert(L"dxgi_local_budget_bytes",JsonValue::CreateNumberValue(static_cast<double>(memory.Budget)));
    }
    JsonArray cases;
    std::vector<std::uint32_t> output;
    cases.Append(gpu.run({16,32,2048},pearl::probe_matrices({16,32,2048},0x1234),output,true,4));
    cases.Append(gpu.run({32,48,4096},pearl::probe_matrices({32,48,4096},0x9876),output,true,4));
    cases.Append(gpu.run({128,128,4096},pearl::probe_matrices({128,128,4096},0xabcd),output,true,4));
    result.Insert(L"cases",cases);
    return result;
}

GpuWorkResult run_gpu_work(const pearl::Shape& shape,const pearl::Matrices& input,bool compare_cpu) {
    static thread_local std::unique_ptr<Gpu> gpu;
    if(!gpu)gpu=std::make_unique<Gpu>();
    GpuWorkResult result;
    result.measurements=gpu->run(shape,input,result.output,compare_cpu);
    result.measurements.Insert(L"software_adapter",JsonValue::CreateBooleanValue(false));
    return result;
}
