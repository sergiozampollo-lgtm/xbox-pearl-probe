#include "pearl_proof.h"
#include "blake3_impl.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace pearl {
namespace {
void u32(std::uint8_t* p, std::uint32_t x) {
    for (unsigned i=0;i<4;++i) p[i]=static_cast<std::uint8_t>(x>>(i*8));
}
std::uint32_t read32(const std::uint8_t* p) {
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
void u64(Bytes& b,std::uint64_t x) {
    for (unsigned i=0;i<8;++i) b.push_back(static_cast<std::uint8_t>(x>>(i*8)));
}
Hash compress(const Hash& key,const std::uint8_t* data,std::uint64_t counter,bool parent,bool root) {
    std::uint32_t cv[8];
    for(unsigned i=0;i<8;++i) cv[i]=read32(key.data()+4*i);
    const unsigned blocks=parent?1:16;
    for(unsigned i=0;i<blocks;++i) {
        const unsigned flags=KEYED_HASH|(parent?PARENT:((i==0?CHUNK_START:0)|(i==15?CHUNK_END:0)))|(root?ROOT:0);
        blake3_compress_in_place(cv,data+i*64,64,counter,static_cast<std::uint8_t>(flags));
    }
    Hash out{};for(unsigned i=0;i<8;++i)u32(out.data()+4*i,cv[i]);return out;
}
Hash parent_cv(const Hash& key,const Hash& a,const Hash& b,bool root) {
    std::array<std::uint8_t,64> block{};
    std::copy(a.begin(),a.end(),block.begin());std::copy(b.begin(),b.end(),block.begin()+32);
    return compress(key,block.data(),0,true,root);
}
Matrices signals(const Shape& s,const Hash& entropy) {
    Matrices result{std::vector<std::int8_t>(std::size_t(s.m)*s.k),std::vector<std::int8_t>(std::size_t(s.n)*s.k)};
    for(unsigned t=0;t<2;++t) {
        auto& bytes=t?result.bt:result.a;
        blake3_hasher h;blake3_hasher_init_keyed(&h,entropy.data());
        const std::uint8_t domain=static_cast<std::uint8_t>(t);blake3_hasher_update(&h,&domain,1);
        Bytes raw(bytes.size());blake3_hasher_finalize(&h,raw.data(),raw.size());
        for(std::size_t i=0;i<raw.size();++i)bytes[i]=static_cast<std::int8_t>(int(raw[i]&127)-64);
    }
    return result;
}
Hash noise_block(const Hash& key,char label,std::uint32_t idx,unsigned slot) {
    std::array<std::uint8_t,64> msg{};u32(msg.data()+4*slot,idx+1);
    const char text[]="A_tensor";std::copy(text,text+8,msg.begin()+32);msg[32]=static_cast<std::uint8_t>(label);
    return digest(msg.data(),msg.size(),&key);
}
std::vector<std::int8_t> add_noise(const std::vector<std::int8_t>& raw,std::uint32_t rows,std::uint32_t k,const Hash& key,char label) {
    std::vector<std::array<std::uint32_t,2>> perm(k);
    for(std::uint32_t i=0;i<k/8;++i) {
        const auto h=noise_block(key,label,i,1);
        for(unsigned j=0;j<8;++j) {
            auto v=read32(h.data()+4*j);auto a=v&127;
            auto b=a^(1+static_cast<std::uint32_t>((std::uint64_t(127)*v)>>32));
            perm[i*8+j]={a,b};
        }
    }
    std::vector<std::int8_t> out(raw.size());
    for(std::uint32_t row=0;row<rows;++row) {
        std::array<int,128> uniform{};
        for(unsigned block=0;block<4;++block) {
            auto hash=noise_block(key,label,row*4+block,0);
            for(unsigned j=0;j<32;++j)uniform[block*32+j]=int(hash[j]&63)-32;
        }
        for(std::uint32_t col=0;col<k;++col) {
            const auto pos=std::size_t(row)*k+col;
            const int value=int(raw[pos])+uniform[perm[col][0]]-uniform[perm[col][1]];
            if(value < -128 || value >127) throw std::runtime_error("noised value outside int8");
            out[pos]=static_cast<std::int8_t>(value);
        }
    }
    return out;
}
}

Bytes decode_hex(const std::string& s) {
    if(s.size()%2)throw std::invalid_argument("odd hex size");
    Bytes b(s.size()/2);
    auto nibble=[](char c)->unsigned {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        throw std::invalid_argument("invalid hex");
    };
    for(std::size_t i=0;i<b.size();++i)b[i]=static_cast<std::uint8_t>((nibble(s[i*2])<<4)|nibble(s[i*2+1]));
    return b;
}
std::string encode_hex(const void* raw,std::size_t n) {
    const auto* p=static_cast<const std::uint8_t*>(raw);constexpr char h[]="0123456789abcdef";
    std::string out(n*2,'0');for(std::size_t i=0;i<n;++i){out[i*2]=h[p[i]>>4];out[i*2+1]=h[p[i]&15];}return out;
}
Header header_from_hex(const std::string& s) {
    auto b=decode_hex(s);if(b.size()!=76)throw std::invalid_argument("header must be 76 bytes");
    Header h{};std::copy(b.begin(),b.end(),h.begin());return h;
}
std::array<std::uint8_t,52> mining_config(const Shape& s) {
    s.validate_probe();
    if(s.k%1024)throw std::invalid_argument("proof generation requires chunk-aligned rows");
    std::array<std::uint8_t,52> c{};u32(c.data(),s.k);c[4]=128;
    c[9]=15;c[15]=15; // Both canonical patterns are contiguous indices 0..15.
    return c;
}
Hash compute_job_key(const Header& header,const Shape& shape) {
    const auto c=mining_config(shape);std::array<std::uint8_t,128> data{};
    std::copy(header.begin(),header.end(),data.begin());std::copy(c.begin(),c.end(),data.begin()+76);
    return digest(data.data(),data.size());
}

MatrixTree::MatrixTree(const std::vector<std::int8_t>& raw,const Hash& key) : data_(raw.begin(),raw.end()) {
    if(data_.size()<2048 || data_.size()%1024)throw std::invalid_argument("tree requires at least two complete chunks");
    std::vector<Hash> leaves(data_.size()/1024);
    for(std::size_t i=0;i<leaves.size();++i)leaves[i]=compress(key,data_.data()+i*1024,i,false,false);
    layers_.push_back(std::move(leaves));
    while(layers_.back().size()>1) {
        const auto& last=layers_.back();std::vector<Hash> next;
        for(std::size_t i=0;i<last.size();i+=2)
            next.push_back(i+1==last.size()?last[i]:parent_cv(key,last[i],last[i+1],last.size()==2));
        layers_.push_back(std::move(next));
    }
    if(root()!=digest(data_.data(),data_.size(),&key))throw std::runtime_error("Merkle root disagrees with official BLAKE3 digest");
}
Bytes MatrixTree::open_rows(std::uint32_t first,std::uint32_t k) const {
    if(k%1024 || (std::uint64_t(first)+16)*k>data_.size())throw std::invalid_argument("row opening out of bounds");
    std::set<std::size_t> indices;
    for(std::size_t i=std::size_t(first)*k/1024;i<std::size_t(first+16)*k/1024;++i)indices.insert(i);
    Bytes out;u64(out,indices.size());
    for(auto i:indices) {
        u64(out,1024); // serde_chunk_vec encodes each leaf as a length-prefixed byte slice.
        out.insert(out.end(),data_.begin()+i*1024,data_.begin()+(i+1)*1024);
    }
    u64(out,indices.size());for(auto i:indices)u64(out,i);
    u64(out,layers_[0].size());out.insert(out.end(),root().begin(),root().end());
    std::vector<Hash> siblings;
    for(std::size_t layer=0;layer+1<layers_.size();++layer) {
        const auto& nodes=layers_[layer];std::set<std::size_t> parents;
        for(auto i:indices) {
            const auto sibling=i^1;
            if(sibling<nodes.size() && !indices.count(sibling))siblings.push_back(nodes[sibling]);
            parents.insert(i/2);
        }
        indices=std::move(parents);
    }
    u64(out,siblings.size());for(const auto& h:siblings)out.insert(out.end(),h.begin(),h.end());
    u64(out,16);for(std::uint32_t row=first;row<first+16;++row)u64(out,row);
    return out;
}
DenseWork::DenseWork(const Header& h,const Shape& s,const Hash& entropy)
 : shape(s),job_key(compute_job_key(h,s)),raw(signals(s,entropy)),a_tree(raw.a,job_key),b_tree(raw.bt,job_key),
   seeds(seeds_v3(job_key,a_tree.root(),b_tree.root(),s.m,s.n)),
   noised{add_noise(raw.a,s.m,s.k,seeds.a,'A'),add_noise(raw.bt,s.n,s.k,seeds.b,'B')} {}
Bytes DenseWork::proof(std::uint32_t ty,std::uint32_t tx) const {
    if(ty>=shape.m/16 || tx>=shape.n/16)throw std::invalid_argument("tile out of bounds");
    Bytes out;u64(out,shape.m);u64(out,shape.n);u64(out,shape.k);u64(out,shape.rank);
    const auto a=a_tree.open_rows(ty*16,shape.k),b=b_tree.open_rows(tx*16,shape.k);
    out.insert(out.end(),a.begin(),a.end());out.insert(out.end(),b.begin(),b.end());out.push_back(0); // None MoE
    return out;
}
Transcript DenseWork::transcript(const std::vector<std::uint32_t>& out,std::size_t tile) const {
    const bool compact=out.size()==shape.tiles()*16;
    if((!compact && out.size()!=shape.output_words())||tile>=shape.tiles())throw std::invalid_argument("invalid GPU output shape");
    const auto offset=compact?0:shape.cells();
    Transcript t{};std::copy_n(out.begin()+offset+tile*16,16,t.begin());return t;
}
bool meets_base_target(const Hash& hash,const Hash& target,const Shape& s) {
    s.validate_probe();Hash bound{};std::uint64_t carry=0;
    const std::uint64_t factor=std::uint64_t(256)*s.k;
    for(unsigned i=0;i<32;++i){carry+=std::uint64_t(target[31-i])*factor;bound[i]=static_cast<std::uint8_t>(carry);carry>>=8;}
    if(carry)return true; // Saturating multiplication, as in the official verifier.
    for(int i=31;i>=0;--i){if(hash[i]<bound[i])return true;if(hash[i]>bound[i])return false;}
    return true;
}
} // namespace pearl
