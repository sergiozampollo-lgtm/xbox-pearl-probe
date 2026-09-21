#include "MiningSession.h"
#include "GpuProbe.h"
#include "pearl_proof.h"
#include "work_queue.h"
#include <Windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <chrono>
#include <cmath>
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
std::optional<double> process_cpu_seconds() {
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return {};
    const auto ticks=[](FILETIME t) { return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; };
    return static_cast<double>(ticks(kernel)+ticks(user))/10000000.0;
}
void string_field(JsonObject& o,const wchar_t* k,const std::string& v) { o.Insert(k,JsonValue::CreateStringValue(to_hstring(v))); }
void number_field(JsonObject& o,const wchar_t* k,double v) { o.Insert(k,JsonValue::CreateNumberValue(v)); }
void save_text(const wchar_t* name,const JsonObject& o) {
    auto folder=ApplicationData::Current().LocalFolder();
    auto f=folder.CreateFileAsync(hstring(name)+L".tmp",CreationCollisionOption::ReplaceExisting).get();
    FileIO::WriteTextAsync(f,o.Stringify()).get();
    f.RenameAsync(name,NameCollisionOption::ReplaceExisting).get();
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
    // Connection policy (private config, with defaults). The pool sends nothing between
    // jobs; a silent socket is NOT an error until receive_idle_limit, and a job stays
    // valid until replaced or job_max_age (pool "stale" replies calibrate this).
    std::chrono::seconds receive_idle_limit{600};
    std::chrono::seconds job_max_age{900};
    Clock::time_point last_message=Clock::now();
    std::uint64_t messages=0,notifies=0,unknown_methods=0;
    std::ofstream log; // pearl-pool-log.jsonl in LocalState: method/id/result/bytes/dt only, no account data
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

    void log_line(const std::string& kind,double id,const std::string& detail,std::size_t bytes) {
        if(!log.is_open())return;
        const auto now=Clock::now();
        log<<"{\"t\":"<<std::chrono::duration<double>(now.time_since_epoch()).count()
           <<",\"dt\":"<<std::chrono::duration<double>(now-last_message).count()
           <<",\"kind\":\""<<kind<<"\",\"id\":"<<id<<",\"bytes\":"<<bytes<<",\"detail\":\""<<detail<<"\"}\n";
        log.flush();
    }
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
        const auto idle=config.GetNamedNumber(L"receive_idle_seconds",600);
        const auto age=config.GetNamedNumber(L"job_max_age_seconds",900);
        if(!std::isfinite(idle)||idle<45||idle>3600||!std::isfinite(age)||age<60||age>3600)
            throw std::runtime_error("receive_idle_seconds must be 45..3600 and job_max_age_seconds 60..3600");
        receive_idle_limit=std::chrono::seconds(static_cast<long long>(idle));
        job_max_age=std::chrono::seconds(static_cast<long long>(age));
        log.open(to_string(ApplicationData::Current().LocalFolder().Path())+"\\pearl-pool-log.jsonl",std::ios::app);
        socket.Control().KeepAlive(true);
        auto connect=socket.ConnectAsync(HostName(host),L"8048",SocketProtectionLevel::Tls12);
        if(connect.wait_for(std::chrono::seconds(20))==winrt::Windows::Foundation::AsyncStatus::Started) {
            connect.Cancel();throw std::runtime_error("pool connect timeout");
        }
        connect.get(); // Default system certificate/hostname verification; no exceptions.
        last_message=Clock::now();
        log_line("connected",0,"",0);
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
                // Silence is not failure: keep waiting on the same pending read until the idle
                // limit; TCP keepalive (enabled above) still surfaces a dead peer as an error.
                while(op.wait_for(std::chrono::seconds(45))==winrt::Windows::Foundation::AsyncStatus::Started) {
                    if(dead || Clock::now()-last_message>receive_idle_limit) {
                        op.Cancel();throw std::runtime_error("pool idle limit reached without any message");
                    }
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
                    ++messages;
                    const auto method=to_string(message.GetNamedString(L"method",L""));
                    if(method=="mining.notify") {
                        ++notifies;log_line("notify",0,to_string(message.GetNamedObject(L"params").GetNamedString(L"target",L"")),line.size());
                    } else if(!method.empty()) {
                        ++unknown_methods;log_line("unhandled_method",0,method,line.size()); // e.g. set_difficulty/ping: observe first
                    } else {
                        double response_id=-1;
                        if(message.HasKey(L"id") && message.GetNamedValue(L"id").ValueType()==JsonValueType::Number)response_id=message.GetNamedNumber(L"id");
                        const bool has_error=message.HasKey(L"error") && message.GetNamedValue(L"error").ValueType()!=JsonValueType::Null;
                        log_line("response",response_id,has_error?"error":"ok",line.size());
                    }
                    last_message=Clock::now();
                    if(method=="mining.notify") {
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
        if(!authorized || dead || !job || Clock::now()-job->received>job_max_age)return {};
        return job;
    }
    bool submit(const Job& j,const pearl::Bytes& proof) {
        std::lock_guard<std::mutex> lock(mutex);
        if(dead||!authorized||!job||job->sequence!=j.sequence||Clock::now()-job->received>job_max_age)return false;
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
    const auto gpu_kernel=to_string(config.GetNamedString(L"gpu_kernel",L"fxc_scalar"));
    select_gpu_kernel(gpu_kernel);
    const bool continuous=config.GetNamedBoolean(L"continuous",false);
    const auto worker_count=config.GetNamedNumber(L"preparation_workers",3);
    if(!std::isfinite(worker_count) || worker_count<1 || worker_count>6 || worker_count!=std::floor(worker_count))
        throw std::runtime_error("preparation_workers must be an integer in 1..6");
    const auto workers=static_cast<unsigned>(worker_count);
    const double duration=config.GetNamedNumber(L"run_seconds",continuous?0:120);
    if(!std::isfinite(duration) || (continuous?duration!=0:(duration<1||duration>86400)))
        throw std::runtime_error("continuous sessions require run_seconds=0; bounded sessions require 1 to 86400");
    const auto start=Clock::now();auto last_save=start-std::chrono::seconds(10);
    const auto cpu_start=process_cpu_seconds();
    const auto should_continue=[&] {
        return continuous || std::chrono::duration<double>(Clock::now()-start).count()<duration;
    };
    std::uint64_t batches=0,attempts=0,submitted=0,accepted=0,rejected=0,reconnects=0;
    double work_units=0,gpu_seconds=0;bool sample_saved=false;std::string error;
    double preparation_seconds=0,preparation_wait_seconds=0,gpu_wall_seconds=0,cpu_results_seconds=0;
    std::uint64_t discarded_prepared_work=0,discarded_ready_on_job_change=0;
    double cpu_dispose_seconds=0,no_job_seconds=0,connect_to_first_batch_seconds=0;
    std::uint64_t pool_messages=0,pool_notifies=0,pool_unknown_methods=0;
    JsonObject status;
    auto record=[&](const char* stage,Pool* pool) {
        std::uint64_t s=0,a=0,r=0;bool auth=false;std::string connection_error,response;
        if(pool){std::lock_guard<std::mutex> lock(pool->mutex);s=pool->submitted;a=pool->accepted;r=pool->rejected;auth=pool->authorized;connection_error=pool->error;response=pool->last_response;
            pool_messages=pool->messages;pool_notifies=pool->notifies;pool_unknown_methods=pool->unknown_methods;}
        const auto elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        string_field(status,L"stage",stage);string_field(status,L"error",connection_error.empty()?error:connection_error);
        if(!response.empty())string_field(status,L"last_submit_response",response);
        status.Insert(L"mining_enabled",JsonValue::CreateBooleanValue(true));
        status.Insert(L"continuous",JsonValue::CreateBooleanValue(continuous));
        number_field(status,L"run_seconds",duration);
        number_field(status,L"m",shape.m);number_field(status,L"n",shape.n);number_field(status,L"k",shape.k);
        number_field(status,L"preparation_workers",workers);
        string_field(status,L"gpu_kernel",gpu_kernel);
        status.Insert(L"authorized",JsonValue::CreateBooleanValue(auth));
        status.Insert(L"pool_target_interpretation_confirmed_by_acceptance",JsonValue::CreateBooleanValue(accepted+a>0));
        number_field(status,L"elapsed_seconds",elapsed);number_field(status,L"batches",static_cast<double>(batches));
        number_field(status,L"app_memory_bytes",static_cast<double>(winrt::Windows::System::MemoryManager::AppMemoryUsage()));
        number_field(status,L"app_memory_limit_bytes",static_cast<double>(winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()));
        number_field(status,L"tile_attempts",static_cast<double>(attempts));number_field(status,L"work_units",work_units);
        number_field(status,L"work_units_per_second",elapsed>0?work_units/elapsed:0);
        number_field(status,L"gpu_seconds",gpu_seconds);number_field(status,L"shares_submitted",static_cast<double>(submitted+s));
        number_field(status,L"shares_accepted",static_cast<double>(accepted+a));number_field(status,L"shares_rejected",static_cast<double>(rejected+r));
        number_field(status,L"reconnections",static_cast<double>(reconnects));
        number_field(status,L"cpu_preparation_seconds_sum",preparation_seconds);
        number_field(status,L"preparation_wait_seconds",preparation_wait_seconds);
        number_field(status,L"gpu_wall_seconds",gpu_wall_seconds);
        number_field(status,L"cpu_results_seconds",cpu_results_seconds);
        number_field(status,L"gpu_kernel_duty_percent",elapsed>0?100*gpu_seconds/elapsed:0);
        number_field(status,L"discarded_prepared_work",static_cast<double>(discarded_prepared_work));
        number_field(status,L"discarded_ready_on_job_change",static_cast<double>(discarded_ready_on_job_change));
        number_field(status,L"cpu_dispose_seconds",cpu_dispose_seconds);
        number_field(status,L"no_job_seconds",no_job_seconds);
        number_field(status,L"connect_to_first_batch_seconds",connect_to_first_batch_seconds);
        number_field(status,L"pool_messages",static_cast<double>(pool_messages));
        number_field(status,L"pool_notifies",static_cast<double>(pool_notifies));
        number_field(status,L"pool_unhandled_methods",static_cast<double>(pool_unknown_methods));
        number_field(status,L"reported_logical_processors",std::thread::hardware_concurrency());
        number_field(status,L"cpu_blake3_simd_degree",static_cast<double>(pearl::cpu_blake3_simd_degree()));
        const auto cpu_now=process_cpu_seconds();
        status.Insert(L"process_cpu_time_available",JsonValue::CreateBooleanValue(bool(cpu_start)&&bool(cpu_now)));
        if(cpu_start && cpu_now) {
            number_field(status,L"process_cpu_seconds",*cpu_now-*cpu_start);
            number_field(status,L"process_cpu_core_equivalent",elapsed>0?(*cpu_now-*cpu_start)/elapsed:0);
        }
        save_text(L"pearl-mining-status.json",status);last_save=Clock::now();
    };
    while(should_continue()) {
        record("connecting",nullptr);
        std::unique_ptr<Pool> pool;
        try {pool=std::make_unique<Pool>(config);}
        catch(const hresult_error& e){error=to_string(e.message());}
        catch(const std::exception& e){error=e.what();}
        if(!pool){++reconnects;record("reconnecting",nullptr);std::this_thread::sleep_for(std::chrono::seconds(3));continue;}
        auto preparation=std::make_unique<pearl::WorkQueue>(shape,workers);
        std::uint64_t preparation_generation=0;
        const auto connection_start=Clock::now();bool first_batch_done=false;
        error.clear();
        while(!pool->dead && should_continue()) {
            if(Clock::now()-last_save>std::chrono::seconds(5))record("running",pool.get());
            const auto job=pool->current();
            if(!job){std::this_thread::sleep_for(std::chrono::milliseconds(100));no_job_seconds+=0.1;continue;}
            if(job->sequence!=preparation_generation) {
                com_array<std::uint8_t> random;CryptographicBuffer::CopyToByteArray(CryptographicBuffer::GenerateRandom(32),random);
                pearl::Hash seed{};std::copy(random.begin(),random.end(),seed.begin());
                discarded_ready_on_job_change+=preparation->set_job(job->header,job->sequence,seed);preparation_generation=job->sequence;
            }
            auto phase_start=Clock::now();
            auto prepared=preparation->take_for(std::chrono::milliseconds(50));
            preparation_wait_seconds+=std::chrono::duration<double>(Clock::now()-phase_start).count();
            if(!prepared)continue;
            const auto latest=pool->current();
            if(!latest || prepared->generation!=latest->sequence) {++discarded_prepared_work;continue;}
            const auto& work=*prepared->work;
            preparation_seconds+=prepared->preparation_seconds;
            if(!first_batch_done){first_batch_done=true;connect_to_first_batch_seconds+=std::chrono::duration<double>(Clock::now()-connection_start).count();}
            phase_start=Clock::now();
            const auto gpu=run_gpu_work(shape,work.noised,false);
            gpu_wall_seconds+=std::chrono::duration<double>(Clock::now()-phase_start).count();
            phase_start=Clock::now();
            ++batches;attempts+=shape.tiles();work_units+=static_cast<double>(shape.m)*shape.n*shape.k;
            gpu_seconds+=gpu.measurements.GetNamedArray(L"iterations").GetObjectAt(0).GetNamedNumber(L"gpu_ms")/1000.0;
            // Validate an actual job on CPU before enabling any submissions.
            if(!sample_saved) {
                check_tile(work,gpu,0);
                check_tile(work,gpu,shape.tiles()-1);
                save_bytes(L"pearl-live-header.bin",job->header);save_bytes(L"pearl-live-proof.bin",work.proof(0,0));
                JsonObject sample;string_field(sample,L"job_id",job->id);string_field(sample,L"target_be",pearl::to_hex(job->target));
                string_field(sample,L"jackpot_le",pearl::to_hex(pearl::jackpot_hash(work.transcript(gpu.output,0),work.seeds.a)));
                sample.Insert(L"submitted",JsonValue::CreateBooleanValue(false));save_text(L"pearl-live-sample.json",sample);sample_saved=true;
            }
            // Batched keyed BLAKE3 over the compact readback (16 LE words per tile); identical to
            // jackpot_hash per tile (tests/merkle_batched_test.cpp), one SIMD call per 64 tiles.
            std::vector<pearl::Hash> hashes(shape.tiles());
            pearl::jackpot_hash_many(gpu.output.data(),shape.tiles(),work.seeds.a,hashes.data());
            for(std::size_t tile=0;tile<shape.tiles();++tile) {
                const auto& hash=hashes[tile];
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
            cpu_results_seconds+=std::chrono::duration<double>(Clock::now()-phase_start).count();
            phase_start=Clock::now();
            prepared.reset(); // ~48 MiB of vectors; previously untimed at the end of the iteration
            cpu_dispose_seconds+=std::chrono::duration<double>(Clock::now()-phase_start).count();
        }
        preparation.reset(); // Join CPU workers before disposing of this pool session.
        bool fatal;
        {std::lock_guard<std::mutex> lock(pool->mutex);submitted+=pool->submitted;accepted+=pool->accepted;rejected+=pool->rejected;error=pool->error;fatal=pool->fatal;}
        pool.reset();
        if(fatal){record("stopped_by_protocol_error",nullptr);return status;}
        if(should_continue())++reconnects;
    }
    record("completed_bounded_run",nullptr);return status;
}
