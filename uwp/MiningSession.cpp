#include "MiningSession.h"
#include "GpuProbe.h"
#include "pearl_proof.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Display.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using Clock = std::chrono::steady_clock;

namespace {
void string_field(JsonObject& o,const wchar_t* k,const std::string& v) { o.Insert(k,JsonValue::CreateStringValue(to_hstring(v))); }
void number_field(JsonObject& o,const wchar_t* k,double v) { o.Insert(k,JsonValue::CreateNumberValue(v)); }
void save_text(const wchar_t* name,const JsonObject& o) {
    auto f=ApplicationData::Current().LocalFolder().CreateFileAsync(name,CreationCollisionOption::ReplaceExisting).get();
    FileIO::WriteTextAsync(f,o.Stringify()).get();
}
template<class T> void save_bytes(const wchar_t* name,const T& b) {
    auto f=ApplicationData::Current().LocalFolder().CreateFileAsync(name,CreationCollisionOption::ReplaceExisting).get();
    FileIO::WriteBytesAsync(f,array_view<const std::uint8_t>(b.data(),b.data()+b.size())).get();
}
struct Job {
    std::string id;
    pearl::Header header{};
    pearl::Hash target{};
    Clock::time_point received;
    std::uint64_t sequence=0;
};
struct Pool {
    StreamSocket socket;
    DataReader reader{nullptr};
    DataWriter writer{nullptr};
    std::thread receiver;
    std::mutex mutex;
    std::atomic<bool> dead{false};
    bool authorized=false, fatal=false;
    std::string error,last_response;
    std::optional<Job> job;
    std::set<unsigned> pending;
    unsigned next_id=2;
    std::uint64_t sequence=0,submitted=0,accepted=0,rejected=0;

    void send(const JsonObject& request) {
        writer.WriteString(request.Stringify()+L"\n");
        auto op=writer.StoreAsync();
        if(op.wait_for(std::chrono::seconds(15))==winrt::Windows::Foundation::AsyncStatus::Started) {
            op.Cancel();throw std::runtime_error("pool send timeout");
        }
        op.get();
    }
    Pool(const JsonObject& config) {
        const auto host=config.GetNamedString(L"host");
        const auto name=to_string(host);
        const std::string suffix=".kryptex.network";
        if(name.size()<=suffix.size() || name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0)
            throw std::runtime_error("configured host is outside the selected Kryptex pool");
        const auto port=config.GetNamedNumber(L"port");
        if(port!=8048)throw std::runtime_error("only the verified PRL TLS port is enabled");
        socket.Control().KeepAlive(true);
        auto connect=socket.ConnectAsync(HostName(host),L"8048",SocketProtectionLevel::Tls12);
        if(connect.wait_for(std::chrono::seconds(20))==winrt::Windows::Foundation::AsyncStatus::Started) {
            connect.Cancel();throw std::runtime_error("pool connect timeout");
        }
        connect.get(); // Default system certificate/hostname verification; no exceptions.
        reader=DataReader(socket.InputStream());reader.InputStreamOptions(InputStreamOptions::Partial);
        writer=DataWriter(socket.OutputStream());writer.UnicodeEncoding(UnicodeEncoding::Utf8);
        JsonObject params,request;
        params.Insert(L"wallet",JsonValue::CreateStringValue(config.GetNamedString(L"wallet")));
        params.Insert(L"worker",JsonValue::CreateStringValue(config.GetNamedString(L"worker")));
        params.Insert(L"pass",JsonValue::CreateStringValue(config.GetNamedString(L"pass",L"x")));
        params.Insert(L"agent",JsonValue::CreateStringValue(L"XboxSeriesS-PearlProbe/0.2"));
        number_field(request,L"id",1);string_field(request,L"method","mining.authorize");request.Insert(L"params",params);
        send(request);
        receiver=std::thread([this] { receive(); });
    }
    ~Pool() {
        dead=true;
        try { socket.Close(); } catch(...) {}
        if(receiver.joinable())receiver.join();
    }
    void receive() {
        try {
            init_apartment(apartment_type::multi_threaded);
            std::string buffer;
            while(!dead) {
                auto op=reader.LoadAsync(4096);
                if(op.wait_for(std::chrono::seconds(45))==winrt::Windows::Foundation::AsyncStatus::Started) {
                    op.Cancel();throw std::runtime_error("pool receive timeout");
                }
                const auto size=op.get();if(!size)throw std::runtime_error("pool closed connection");
                std::vector<std::uint8_t> bytes(size);reader.ReadBytes(bytes);
                buffer.append(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                if(buffer.size()>262144)throw std::runtime_error("oversized pool message");
                std::size_t end;
                while((end=buffer.find('\n'))!=std::string::npos) {
                    auto line=buffer.substr(0,end);buffer.erase(0,end+1);
                    const auto message=JsonObject::Parse(to_hstring(line));
                    std::lock_guard<std::mutex> lock(mutex);
                    if(message.GetNamedString(L"method",L"")==L"mining.notify") {
                        const auto p=message.GetNamedObject(L"params");
                        if(p.GetNamedNumber(L"cert_version",0)!=3) {
                            fatal=true;throw std::runtime_error("unsupported Pearl certificate version; mining stopped");
                        }
                        Job j;j.id=to_string(p.GetNamedString(L"job_id"));
                        if(j.id.empty()||j.id.size()>256)throw std::runtime_error("invalid pool job name");
                        j.header=pearl::header_from_hex(to_string(p.GetNamedString(L"header")));
                        j.target=pearl::from_hex(to_string(p.GetNamedString(L"target")));
                        j.received=Clock::now();j.sequence=++sequence;job=j;
                    } else if(message.HasKey(L"id") && message.GetNamedValue(L"id").ValueType()==JsonValueType::Number) {
                        const auto id=message.GetNamedNumber(L"id");
                        const bool ok=message.HasKey(L"result") && message.GetNamedValue(L"result").ValueType()==JsonValueType::Boolean &&
                            message.GetNamedBoolean(L"result") && (!message.HasKey(L"error")||message.GetNamedValue(L"error").ValueType()==JsonValueType::Null);
                        if(id==1) {
                            authorized=ok;if(!ok){fatal=true;throw std::runtime_error("pool authorization rejected");}
                        } else if(id>=2 && id<4294967295.0 && pending.erase(static_cast<unsigned>(id))) {
                            last_response=to_string(message.Stringify());
                            save_text(L"pearl-last-pool-response.json",message);
                            if(ok)++accepted;else {++rejected;error=to_string(message.Stringify());}
                        }
                    }
                }
            }
        }catch(const hresult_error& e){std::lock_guard<std::mutex> lock(mutex);error=to_string(e.message());}
        catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);error=e.what();}
        dead=true;
    }
    std::optional<Job> current() {
        std::lock_guard<std::mutex> lock(mutex);
        if(!authorized || dead || !job || Clock::now()-job->received>std::chrono::seconds(60))return {};
        return job;
    }
    bool submit(const Job& j,const pearl::Bytes& proof) {
        std::lock_guard<std::mutex> lock(mutex);
        if(dead||!authorized||!job||job->sequence!=j.sequence||Clock::now()-job->received>std::chrono::seconds(60))return false;
        if(pending.size()>=8)throw std::runtime_error("pool acknowledgements missing; refusing to accumulate submissions");
        const auto id=next_id++;
        JsonObject p,request;
        string_field(p,L"job_id",j.id);
        p.Insert(L"plain_proof",JsonValue::CreateStringValue(CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::CreateFromByteArray(proof))));
        number_field(request,L"id",id);string_field(request,L"method","mining.submit");request.Insert(L"params",p);
        pending.insert(id);send(request);++submitted;return true;
    }
};
void check_tile(const pearl::DenseWork& work,const GpuWorkResult& gpu,std::size_t tile) {
    const auto y=tile/(work.shape.n/16),x=tile%(work.shape.n/16);
    const auto k=work.shape.k;
    pearl::Matrices subset;
    subset.a.assign(work.noised.a.begin()+y*16*k,work.noised.a.begin()+(y+1)*16*k);
    subset.bt.assign(work.noised.bt.begin()+x*16*k,work.noised.bt.begin()+(x+1)*16*k);
    const auto cpu=pearl::reference_matmul({16,16,k},subset);
    const auto transcript=work.transcript(gpu.output,tile);
    if(!std::equal(transcript.begin(),transcript.end(),cpu.begin()+256))throw std::runtime_error("live GPU tile disagrees with CPU; mining stopped");
}
}

JsonObject run_mining_session(const JsonObject& config) {
    if(!config.GetNamedBoolean(L"mining_enabled",false))throw std::runtime_error("private configuration did not enable mining");
    const pearl::Shape shape{static_cast<std::uint32_t>(config.GetNamedNumber(L"m",256)),
        static_cast<std::uint32_t>(config.GetNamedNumber(L"n",256)),static_cast<std::uint32_t>(config.GetNamedNumber(L"k",4096))};
    shape.validate_probe();(void)pearl::mining_config(shape);
    const double duration=config.GetNamedNumber(L"run_seconds",120);
    if(duration<1||duration>86400)throw std::runtime_error("run_seconds must be between 1 and 86400");
    winrt::Windows::System::Display::DisplayRequest display;
    display.RequestActive(); // Applies only while this foreground app is alive.
    const auto start=Clock::now();auto last_save=start-std::chrono::seconds(10);
    std::uint64_t batches=0,attempts=0,submitted=0,accepted=0,rejected=0,reconnects=0;
    double work_units=0,gpu_seconds=0;bool sample_saved=false;std::string error;
    JsonObject status;
    auto record=[&](const char* stage,Pool* pool) {
        std::uint64_t s=0,a=0,r=0;bool auth=false;std::string connection_error,response;
        if(pool){std::lock_guard<std::mutex> lock(pool->mutex);s=pool->submitted;a=pool->accepted;r=pool->rejected;auth=pool->authorized;connection_error=pool->error;response=pool->last_response;}
        const auto elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        string_field(status,L"stage",stage);string_field(status,L"error",connection_error.empty()?error:connection_error);
        if(!response.empty())string_field(status,L"last_submit_response",response);
        status.Insert(L"mining_enabled",JsonValue::CreateBooleanValue(true));
        status.Insert(L"authorized",JsonValue::CreateBooleanValue(auth));
        status.Insert(L"pool_target_interpretation_confirmed_by_acceptance",JsonValue::CreateBooleanValue(accepted+a>0));
        number_field(status,L"elapsed_seconds",elapsed);number_field(status,L"batches",static_cast<double>(batches));
        number_field(status,L"tile_attempts",static_cast<double>(attempts));number_field(status,L"work_units",work_units);
        number_field(status,L"work_units_per_second",elapsed>0?work_units/elapsed:0);
        number_field(status,L"gpu_seconds",gpu_seconds);number_field(status,L"shares_submitted",static_cast<double>(submitted+s));
        number_field(status,L"shares_accepted",static_cast<double>(accepted+a));number_field(status,L"shares_rejected",static_cast<double>(rejected+r));
        number_field(status,L"reconnections",static_cast<double>(reconnects));
        save_text(L"pearl-mining-status.json",status);last_save=Clock::now();
    };
    while(std::chrono::duration<double>(Clock::now()-start).count()<duration) {
        record("connecting",nullptr);
        std::unique_ptr<Pool> pool;
        try {pool=std::make_unique<Pool>(config);}
        catch(const hresult_error& e){error=to_string(e.message());}
        catch(const std::exception& e){error=e.what();}
        if(!pool){++reconnects;record("reconnecting",nullptr);std::this_thread::sleep_for(std::chrono::seconds(3));continue;}
        while(!pool->dead && std::chrono::duration<double>(Clock::now()-start).count()<duration) {
            if(Clock::now()-last_save>std::chrono::seconds(5))record("running",pool.get());
            const auto job=pool->current();
            if(!job){std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
            com_array<std::uint8_t> random;CryptographicBuffer::CopyToByteArray(CryptographicBuffer::GenerateRandom(32),random);
            pearl::Hash entropy{};std::copy(random.begin(),random.end(),entropy.begin());
            pearl::DenseWork work(job->header,shape,entropy);
            const auto gpu=run_gpu_work(shape,work.noised,false);
            ++batches;attempts+=shape.tiles();work_units+=static_cast<double>(shape.m)*shape.n*shape.k;
            gpu_seconds+=gpu.measurements.GetNamedArray(L"iterations").GetObjectAt(0).GetNamedNumber(L"gpu_ms")/1000.0;
            // Validate an actual job on CPU before enabling any submissions.
            if(!sample_saved) {
                check_tile(work,gpu,0);
                save_bytes(L"pearl-live-header.bin",job->header);save_bytes(L"pearl-live-proof.bin",work.proof(0,0));
                JsonObject sample;string_field(sample,L"job_id",job->id);string_field(sample,L"target_be",pearl::to_hex(job->target));
                string_field(sample,L"jackpot_le",pearl::to_hex(pearl::jackpot_hash(work.transcript(gpu.output,0),work.seeds.a)));
                sample.Insert(L"submitted",JsonValue::CreateBooleanValue(false));save_text(L"pearl-live-sample.json",sample);sample_saved=true;
            }
            for(std::size_t tile=0;tile<shape.tiles();++tile) {
                const auto hash=pearl::jackpot_hash(work.transcript(gpu.output,tile),work.seeds.a);
                if(!pearl::meets_base_target(hash,job->target,shape))continue;
                check_tile(work,gpu,tile);
                const auto proof=work.proof(static_cast<std::uint32_t>(tile/(shape.n/16)),static_cast<std::uint32_t>(tile%(shape.n/16)));
                save_bytes(L"pearl-submitted-header.bin",job->header);save_bytes(L"pearl-submitted-proof.bin",proof);
                JsonObject candidate;string_field(candidate,L"job_id",job->id);string_field(candidate,L"target_be",pearl::to_hex(job->target));
                string_field(candidate,L"jackpot_le",pearl::to_hex(hash));save_text(L"pearl-candidate.json",candidate);
                try {pool->submit(*job,proof);}
                catch(const hresult_error& e){error=to_string(e.message());pool->dead=true;break;}
                catch(const std::exception& e){error=e.what();pool->dead=true;break;}
            }
        }
        bool fatal;
        {std::lock_guard<std::mutex> lock(pool->mutex);submitted+=pool->submitted;accepted+=pool->accepted;rejected+=pool->rejected;error=pool->error;fatal=pool->fatal;}
        pool.reset();
        if(fatal){record("stopped_by_protocol_error",nullptr);return status;}
        if(std::chrono::duration<double>(Clock::now()-start).count()<duration)++reconnects;
    }
    record("completed_bounded_run",nullptr);return status;
}
