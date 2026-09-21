#include "pearl_core.h"
#include "blake3_impl.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace pearl {
std::size_t cpu_blake3_simd_degree() { return blake3_simd_degree(); }

Hash digest(const void* data, std::size_t size, const Hash* key) {
    blake3_hasher state;
    if (key) blake3_hasher_init_keyed(&state, key->data());
    else blake3_hasher_init(&state);
    blake3_hasher_update(&state, data, size);
    Hash out{};
    blake3_hasher_finalize(&state, out.data(), out.size());
    return out;
}

Hash from_hex(const std::string& value) {
    if (value.size() != 64) throw std::invalid_argument("hash must have 64 hex characters");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw std::invalid_argument("invalid hex character");
    };
    Hash out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint8_t>((digit(value[2*i]) << 4) | digit(value[2*i+1]));
    return out;
}

std::string to_hex(const Hash& value) {
    constexpr char chars[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        out[2*i] = chars[value[i] >> 4];
        out[2*i+1] = chars[value[i] & 15];
    }
    return out;
}

namespace {
void store_le(std::uint8_t* dest, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) dest[i] = static_cast<std::uint8_t>(v >> (8*i));
}
Hash bind_root(const Hash& root, std::uint32_t dim, const Hash& salt) {
    std::array<std::uint8_t, 64> message{};
    std::copy(root.begin(), root.end(), message.begin());
    store_le(message.data() + 32, dim);
    return digest(message.data(), message.size(), &salt);
}
Hash chain(const Hash& first, const Hash& second) {
    std::array<std::uint8_t, 64> message{};
    std::copy(first.begin(), first.end(), message.begin());
    std::copy(second.begin(), second.end(), message.begin()+32);
    return digest(message.data(), message.size());
}
}

Seeds seeds_v3(const Hash& key, const Hash& root_a, const Hash& root_b,
               std::uint32_t m, std::uint32_t n) {
    if (m == 0 || n == 0 || m > (1U << 24) || n > (1U << 24))
        throw std::invalid_argument("V3 dimensions outside 1..2^24");
    const auto salt_a = from_hex("8249406ca0ed15169616f692fcf076f892dbdb2a7023b852f0d47719c390017b");
    const auto salt_b = from_hex("11300632ec6301ca2be2af718b3f4d4f1ae9c63988e8cc044844301d71b89aa9");
    Seeds seeds{};
    seeds.b = chain(key, bind_root(root_b, n, salt_b));
    seeds.a = chain(seeds.b, bind_root(root_a, m, salt_a));
    return seeds;
}

Hash jackpot_hash(const Transcript& transcript, const Hash& a_seed) {
    std::array<std::uint8_t, 64> bytes{};
    for (std::size_t i = 0; i < transcript.size(); ++i)
        store_le(bytes.data() + 4*i, transcript[i]);
    return digest(bytes.data(), bytes.size(), &a_seed);
}

void Shape::validate_probe() const {
    // This bound also keeps every signed int8 dot product safely inside int32.
    if (m == 0 || n == 0 || m > 2048 || n > 2048 || m % 16 || n % 16 ||
        k < 2048 || k > 16384 || k % rank || std::uint64_t(m)*n*k > (std::uint64_t(1)<<34))
        throw std::invalid_argument("requires M,N=16..2048 (step 16), K=2048..16384 (step 128), at most 2^34 work units per dispatch");
}
std::size_t Shape::cells() const { return static_cast<std::size_t>(m) * n; }
std::size_t Shape::tiles() const { return static_cast<std::size_t>(m/16) * (n/16); }
std::size_t Shape::output_words() const { return cells() + tiles()*16; }

Matrices probe_matrices(const Shape& s, std::uint32_t seed) {
    s.validate_probe();
    Matrices out{std::vector<std::int8_t>(static_cast<std::size_t>(s.m)*s.k),
                 std::vector<std::int8_t>(static_cast<std::size_t>(s.n)*s.k)};
    // Deterministic test input only. Deliberately covers the entire signed byte
    // range to catch GPU sign-extension errors after noise is eventually added.
    auto next = [&seed]() -> std::int8_t {
        seed = seed * 1664525U + 1013904223U;
        const int v = static_cast<int>(seed >> 24) - 128;
        return static_cast<std::int8_t>(v);
    };
    std::generate(out.a.begin(), out.a.end(), next);
    std::generate(out.bt.begin(), out.bt.end(), next);
    return out;
}

void transcript_accumulate(Transcript& data, std::uint32_t round, std::uint32_t value) {
    auto& slot = data[round % data.size()];
    slot = ((slot << 13) | (slot >> 19)) ^ value;
}

std::vector<std::uint32_t> reference_matmul(const Shape& s, const Matrices& inputs) {
    s.validate_probe();
    if (inputs.a.size() != static_cast<std::size_t>(s.m)*s.k ||
        inputs.bt.size() != static_cast<std::size_t>(s.n)*s.k)
        throw std::invalid_argument("matrix storage does not match shape");
    std::vector<std::uint32_t> out(s.output_words(), 0);
    for (std::uint32_t ty = 0; ty < s.m/16; ++ty) {
        for (std::uint32_t tx = 0; tx < s.n/16; ++tx) {
            std::array<std::int32_t, 256> sums{};
            Transcript transcript{};
            for (std::uint32_t start = 0; start < s.k; start += s.rank) {
                std::uint32_t reduction = 0;
                for (std::uint32_t y = 0; y < 16; ++y) {
                    const auto* a = inputs.a.data() + static_cast<std::size_t>(ty*16+y)*s.k + start;
                    for (std::uint32_t x = 0; x < 16; ++x) {
                        const auto* b = inputs.bt.data() + static_cast<std::size_t>(tx*16+x)*s.k + start;
                        auto& sum = sums[y*16+x];
                        for (std::uint32_t d = 0; d < s.rank; ++d)
                            sum += static_cast<std::int32_t>(a[d]) * static_cast<std::int32_t>(b[d]);
                        reduction ^= static_cast<std::uint32_t>(sum);
                    }
                }
                transcript_accumulate(transcript, start/s.rank, reduction);
            }
            for (std::uint32_t y = 0; y < 16; ++y)
                for (std::uint32_t x = 0; x < 16; ++x)
                    out[static_cast<std::size_t>(ty*16+y)*s.n + tx*16+x] = static_cast<std::uint32_t>(sums[y*16+x]);
            const auto tile = static_cast<std::size_t>(ty)*(s.n/16)+tx;
            std::copy(transcript.begin(), transcript.end(), out.begin()+s.cells()+tile*16);
        }
    }
    return out;
}
} // namespace pearl
