#pragma once
#include "pearl_proof.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace pearl {
struct PreparedWork {
    std::uint64_t generation;
    Hash entropy;
    double preparation_seconds;
    std::unique_ptr<DenseWork> work;
};

// CPU preparation only. No network, GPU calls, or background auto-start.
// Both in-flight work and queued work count toward the fixed memory bound.
class WorkQueue {
public:
    WorkQueue(const Shape&, unsigned workers);
    ~WorkQueue();
    WorkQueue(const WorkQueue&)=delete;
    WorkQueue& operator=(const WorkQueue&)=delete;
    // Returns how many already-prepared items of the previous job were discarded.
    std::size_t set_job(const Header&, std::uint64_t generation, const Hash& entropy_seed);
    std::unique_ptr<PreparedWork> take_for(std::chrono::milliseconds);
private:
    void produce();
    void stop();
    Shape shape_;
    std::size_t capacity_,in_flight_=0;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_=false,has_job_=false;
    Header header_{};
    Hash seed_{};
    std::uint64_t generation_=0,counter_=0;
    std::exception_ptr failure_;
    std::deque<std::unique_ptr<PreparedWork>> ready_;
    std::vector<std::thread> threads_;
};
} // namespace pearl
