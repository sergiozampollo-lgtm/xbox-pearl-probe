#include "work_queue.h"
#include <stdexcept>

namespace pearl {
WorkQueue::WorkQueue(const Shape& shape,unsigned workers):shape_(shape),capacity_(workers+1) {
    shape_.validate_probe();
    if(workers<1 || workers>6)throw std::invalid_argument("CPU preparation workers must be 1..6");
    threads_.reserve(workers);
    try {
        for(unsigned i=0;i<workers;++i)threads_.emplace_back([this]{produce();});
    } catch(...) { stop();throw; }
}
void WorkQueue::stop() {
    { std::lock_guard<std::mutex> lock(mutex_);stopping_=true; }
    changed_.notify_all();
    for(auto& thread:threads_)if(thread.joinable())thread.join();
}
WorkQueue::~WorkQueue() { stop(); }
void WorkQueue::set_job(const Header& header,std::uint64_t generation,const Hash& seed) {
    if(!generation)throw std::invalid_argument("job generation must be nonzero");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(has_job_ && generation<=generation_)throw std::invalid_argument("job generations must increase");
        header_=header;generation_=generation;seed_=seed;has_job_=true;
        ready_.clear(); // Completed work from the previous task cannot escape.
    }
    changed_.notify_all();
}
std::unique_ptr<PreparedWork> WorkQueue::take_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock,timeout,[&]{return stopping_ || failure_ || !ready_.empty();});
    if(failure_)std::rethrow_exception(failure_);
    if(ready_.empty())return {};
    auto result=std::move(ready_.front());ready_.pop_front();
    lock.unlock();changed_.notify_all();return result;
}
void WorkQueue::produce() {
    for(;;) {
        Header header;Hash seed;std::uint64_t generation,counter;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock,[&]{return stopping_ || failure_ || (has_job_ && ready_.size()+in_flight_<capacity_);});
            if(stopping_ || failure_)return;
            header=header_;seed=seed_;generation=generation_;counter=++counter_;++in_flight_;
        }
        bool counted=true;
        try {
            const auto start=std::chrono::steady_clock::now();
            std::array<std::uint8_t,16> message{};
            for(unsigned i=0;i<8;++i) {
                message[i]=static_cast<std::uint8_t>(generation>>(i*8));
                message[i+8]=static_cast<std::uint8_t>(counter>>(i*8));
            }
            const auto entropy=digest(message.data(),message.size(),&seed);
            auto work=std::make_unique<DenseWork>(header,shape_,entropy);
            const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            auto result=std::make_unique<PreparedWork>(PreparedWork{generation,entropy,seconds,std::move(work)});
            {
                std::lock_guard<std::mutex> lock(mutex_);--in_flight_;counted=false;
                if(stopping_)return;
                if(generation==generation_)ready_.push_back(std::move(result));
            }
            changed_.notify_all();
        } catch(...) {
            { std::lock_guard<std::mutex> lock(mutex_);if(counted)--in_flight_;failure_=std::current_exception(); }
            changed_.notify_all();return;
        }
    }
}
} // namespace pearl
