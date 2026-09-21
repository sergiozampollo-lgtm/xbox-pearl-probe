#include "pearl_proof.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("usage: pearl_proof_fixture output_directory");
        const std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);
        pearl::Header header{};header[0]=1;header[72]=0xff;header[73]=0xff;header[74]=0x7f;header[75]=0x20;
        auto write=[&](const std::string& name,const auto& b){std::ofstream f(out/name,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("write failed");};
        write("header.bin",header);
        for(unsigned test=0;test<3;++test) {
            const pearl::Shape shape=test==0?pearl::Shape{16,32,2048}:test==1?pearl::Shape{32,48,4096}:pearl::Shape{64,96,2048};
            pearl::Hash entropy{};entropy[0]=static_cast<std::uint8_t>(test+1);
            pearl::DenseWork work(header,shape,entropy);
            const auto output=pearl::reference_matmul(shape,work.noised);
            const auto ty=shape.m/16-1,tx=shape.n/16-1;
            const auto t=work.transcript(output,std::size_t(ty)*(shape.n/16)+tx);
            const auto hash=pearl::jackpot_hash(t,work.seeds.a);
            write("proof-"+std::to_string(test)+".bin",work.proof(ty,tx));
            std::cout << "{\"case\":" << test << ",\"jackpot_le\":\"" << pearl::to_hex(hash) << "\",\"job_key\":\"" << pearl::to_hex(work.job_key) << "\",\"a_seed\":\"" << pearl::to_hex(work.seeds.a) << "\",\"transcript\":[";
            for(unsigned i=0;i<16;++i)std::cout<<(i?",":"")<<t[i];
            std::cout<<"]}\n";
        }
        pearl::Hash zero{},max{};max.fill(255);pearl::Hash base{};base[31]=1;
        pearl::Hash limit{};limit[2]=8;const pearl::Shape shape{16,16,2048};
        if(!pearl::meets_base_target(zero,zero,shape)||pearl::meets_base_target(max,zero,shape)||!pearl::meets_base_target(max,max,shape)||!pearl::meets_base_target(limit,base,shape))throw std::runtime_error("target boundary failed");
        limit[0]=1;if(pearl::meets_base_target(limit,base,shape))throw std::runtime_error("target above boundary accepted");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
