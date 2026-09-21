#include "ProofProbe.h"
#include "GpuProbe.h"
#include "pearl_proof.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Storage;

namespace {
template<class T> void save(const wchar_t* name,const T& bytes) {
    auto file=ApplicationData::Current().LocalFolder().CreateFileAsync(name,CreationCollisionOption::ReplaceExisting).get();
    FileIO::WriteBytesAsync(file,winrt::array_view<const std::uint8_t>(bytes.data(),bytes.data()+bytes.size())).get();
}
}
JsonObject run_proof_probe() {
    pearl::Header header{};header[0]=1;header[72]=0xff;header[73]=0xff;header[74]=0x7f;header[75]=0x20;
    save(L"pearl-proof-header.bin",header);
    JsonObject result;JsonArray cases;
    for(unsigned test=0;test<3;++test) {
        const pearl::Shape shape=test==0?pearl::Shape{16,32,2048}:test==1?pearl::Shape{32,48,4096}:pearl::Shape{64,96,2048};
        pearl::Hash entropy{};entropy[0]=static_cast<std::uint8_t>(test+1);
        pearl::DenseWork work(header,shape,entropy);
        auto gpu=run_gpu_work(shape,work.noised,true);
        const auto ty=shape.m/16-1,tx=shape.n/16-1;
        const auto transcript=work.transcript(gpu.output,std::size_t(ty)*(shape.n/16)+tx);
        const auto proof=work.proof(ty,tx);
        const auto name=L"pearl-proof-"+std::to_wstring(test)+L".bin";
        save(name.c_str(),proof);
        JsonObject c;
        c.Insert(L"case",JsonValue::CreateNumberValue(test));
        c.Insert(L"jackpot_le",JsonValue::CreateStringValue(winrt::to_hstring(pearl::to_hex(pearl::jackpot_hash(transcript,work.seeds.a)))));
        c.Insert(L"proof_blake3",JsonValue::CreateStringValue(winrt::to_hstring(pearl::to_hex(pearl::digest(proof.data(),proof.size())))));
        c.Insert(L"proof_bytes",JsonValue::CreateNumberValue(static_cast<double>(proof.size())));
        c.Insert(L"gpu",gpu.measurements);cases.Append(c);
    }
    result.Insert(L"cases",cases);
    result.Insert(L"live_target_checked",JsonValue::CreateBooleanValue(false));
    return result;
}
