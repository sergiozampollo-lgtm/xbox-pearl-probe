// Batched Merkle construction, incremental chunk update, batched jackpot scan and the
// B-reuse seed property, all checked against the official hasher / scalar reference.
#include "pearl_proof.h"
#include "blake3_impl.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace pearl;
namespace {
void require(bool ok,const char* m){if(!ok)throw std::runtime_error(m);}
std::uint32_t rd32(const std::uint8_t* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
// Independent scalar reference: one compress_in_place per block, as in v0.1.0.16.
Hash ref_compress(const Hash& key,const std::uint8_t* data,std::uint64_t counter,bool parent,bool root){
    std::uint32_t cv[8];for(unsigned i=0;i<8;++i)cv[i]=rd32(key.data()+4*i);
    const unsigned blocks=parent?1:16;
    for(unsigned i=0;i<blocks;++i){const unsigned f=KEYED_HASH|(parent?PARENT:((i==0?CHUNK_START:0)|(i==15?CHUNK_END:0)))|(root?ROOT:0);
        blake3_compress_in_place(cv,data+i*64,64,counter,static_cast<std::uint8_t>(f));}
    Hash out{};for(unsigned i=0;i<8;++i)for(unsigned b=0;b<4;++b)out[4*i+b]=static_cast<std::uint8_t>(cv[i]>>(8*b));return out;}
Hash ref_root(const std::vector<std::int8_t>& raw,const Hash& key){
    std::vector<Hash> layer(raw.size()/1024);
    for(std::size_t i=0;i<layer.size();++i)layer[i]=ref_compress(key,reinterpret_cast<const std::uint8_t*>(raw.data())+i*1024,i,false,false);
    while(layer.size()>1){std::vector<Hash> next;
        for(std::size_t i=0;i<layer.size();i+=2){
            if(i+1==layer.size()){next.push_back(layer[i]);continue;}
            std::array<std::uint8_t,64> block{};std::copy(layer[i].begin(),layer[i].end(),block.begin());std::copy(layer[i+1].begin(),layer[i+1].end(),block.begin()+32);
            next.push_back(ref_compress(key,block.data(),0,true,layer.size()==2));}
        layer=std::move(next);}
    return layer[0];}
}
int main(){
    try{
        Hash key{};key.fill(0x42);
        // Trees of 2..70 chunks (even, odd, power of two, non power of two).
        for(std::size_t chunks:{2,3,5,8,13,64,70}){
            std::vector<std::int8_t> raw(chunks*1024);for(std::size_t i=0;i<raw.size();++i)raw[i]=static_cast<std::int8_t>((i*31+chunks)%251);
            MatrixTree tree(raw,key);
            require(tree.root()==ref_root(raw,key),"batched root differs from scalar reference");
            require(tree.verify_root(),"batched root differs from official digest");
            // Incremental update of a middle chunk and of the last chunk.
            for(std::size_t idx:{chunks/2,chunks-1}){
                std::vector<std::int8_t> chunk(raw.begin()+idx*1024,raw.begin()+(idx+1)*1024);chunk[7]^=0x55;chunk[1000]^=0x01;
                std::copy(chunk.begin(),chunk.end(),raw.begin()+idx*1024);
                tree.update_chunk(idx,chunk.data());
                require(tree.root()==ref_root(raw,key),"incremental root differs from full reference");
                require(tree.verify_root(),"incremental root differs from official digest");
                if(chunks>=16)require(tree.open_rows(0,1024)==MatrixTree(raw,key).open_rows(0,1024),"opening differs after incremental update");
            }
        }
        // Batched jackpot scan equals the scalar API for every tile.
        const Shape s{144,144,2048};Hash seed{};seed[3]=9;
        std::vector<std::uint32_t> words(s.tiles()*16);for(std::size_t i=0;i<words.size();++i)words[i]=static_cast<std::uint32_t>(i*2654435761u)^static_cast<std::uint32_t>(i>>3);
        std::vector<Hash> many(s.tiles());jackpot_hash_many(words.data(),s.tiles(),seed,many.data());
        for(std::size_t t=0;t<s.tiles();++t){Transcript tr{};std::copy_n(words.begin()+t*16,16,tr.begin());require(many[t]==jackpot_hash(tr,seed),"jackpot_hash_many differs");}
        // Seed chain: a new root_A leaves seeds.b (hence B's noise) unchanged.
        Header h{};h[0]=1;Hash e1{},e2{};e1[0]=1;e2[0]=2;
        DenseWork w1(h,s,e1,true),w2(h,s,e2,true);
        const auto sb1=seeds_v3(w1.job_key,w1.a_tree.root(),w1.b_tree.root(),s.m,s.n),sb2=seeds_v3(w1.job_key,w2.a_tree.root(),w1.b_tree.root(),s.m,s.n);
        require(sb1.b==sb2.b && sb1.a!=sb2.a,"seed chain: B seed must not depend on root_A");
        std::cout<<"Batched Merkle, incremental update, batched jackpot scan and seed chain passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
