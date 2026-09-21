#include "work_queue.h"
#include <iostream>
#include <set>
#include <stdexcept>

void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
int main() {
    try {
        const pearl::Shape shape{16,32,2048};pearl::Header header{};pearl::Hash seed{};
        header[0]=1;seed[0]=17;
        // Offline deterministic preparation; no live jobs or mining on this host.
        pearl::WorkQueue queue(shape,3);
        require(!queue.take_for(std::chrono::milliseconds(5)),"empty queue did not time out");
        queue.set_job(header,1,seed);
        std::set<pearl::Hash> nonces;
        for(unsigned i=0;i<8;++i) {
            auto item=queue.take_for(std::chrono::seconds(5));
            require(bool(item),"preparation timed out");
            require(item->generation==1 && nonces.insert(item->entropy).second,"duplicate or stale prepared work");
            const pearl::DenseWork serial(header,shape,item->entropy);
            require(item->work->noised.a==serial.noised.a && item->work->noised.bt==serial.noised.bt,"parallel preparation differs from serial");
            require(item->work->proof(0,1)==serial.proof(0,1),"parallel proof differs from serial");
        }
        header[1]=9;queue.set_job(header,2,seed);
        auto fresh=queue.take_for(std::chrono::seconds(5));
        require(fresh && fresh->generation==2,"obsolete task survived job replacement");
        require(fresh->work->job_key==pearl::compute_job_key(header,shape),"wrong job header");
        { pearl::WorkQueue waiting(shape,2); } // Destruction wakes idle workers.
        { pearl::WorkQueue busy(shape,2);busy.set_job(header,1,seed); } // Joins active workers.
        std::cout<<"Parallel preparation, nonce uniqueness, job replacement and shutdown passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
