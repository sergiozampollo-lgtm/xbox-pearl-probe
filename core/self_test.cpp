#include "pearl_core.h"
#include "blake3_vectors.h"
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace pearl {
unsigned self_test() {
    unsigned checks = 0;
    auto check = [&checks](bool ok, const std::string& name) {
        if (!ok) throw std::runtime_error("self-test failed: " + name);
        ++checks;
    };
    Hash vector_key{};
    constexpr std::string_view key_text = "whats the Elvish word for friend";
    static_assert(key_text.size() == 32);
    std::copy(key_text.begin(), key_text.end(), vector_key.begin());
    for (const auto& vector : blake3_vectors) {
        std::vector<std::uint8_t> input(vector.size);
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>(i % 251);
        check(to_hex(digest(input.data(), input.size())) == vector.hash,
              "BLAKE3 hash length " + std::to_string(vector.size));
        check(to_hex(digest(input.data(), input.size(), &vector_key)) == vector.keyed,
              "BLAKE3 keyed length " + std::to_string(vector.size));
    }
    const std::string salt_a = "pearl/cert-v3/noise-seed/A";
    const std::string salt_b = "pearl/cert-v3/noise-seed/B";
    check(to_hex(digest(salt_a.data(), salt_a.size())) ==
          "8249406ca0ed15169616f692fcf076f892dbdb2a7023b852f0d47719c390017b", "Pearl V3 salt A");
    check(to_hex(digest(salt_b.data(), salt_b.size())) ==
          "11300632ec6301ca2be2af718b3f4d4f1ae9c63988e8cc044844301d71b89aa9", "Pearl V3 salt B");
    Hash key{}, a{}, b{};
    key.fill(0x11); a.fill(0xaa); b.fill(0xbb);
    const auto seeds = seeds_v3(key, a, b, 192, 320);
    check(to_hex(seeds.a) == "301784168005ec833ab0aa60006f7fe7faaa95307d8c1fc6819b2ffdd717eccf", "Pearl upstream V3 seed A");
    check(to_hex(seeds.b) == "60ed9b73c5a9599b200b6cd563e7f0d5d9a67d2402d85fd4ef966c580080d0e5", "Pearl upstream V3 seed B");
    const auto changed_m = seeds_v3(key, a, b, 193, 320);
    const auto changed_n = seeds_v3(key, a, b, 192, 321);
    check(changed_m.a != seeds.a && changed_m.b == seeds.b, "V3 commits M to A");
    check(changed_n.a != seeds.a && changed_n.b != seeds.b, "V3 commits N to both seeds");
    try { (void)seeds_v3(key,a,b,0,320); check(false, "reject zero dimension"); }
    catch (const std::invalid_argument&) { ++checks; }

    // Exact edge cases verify cumulative (not per-chunk) reductions, signed
    // arithmetic, LE words and transcript wrap after 16 rank-sized reductions.
    const Shape shape{16,16,4096};
    Matrices inputs{std::vector<std::int8_t>(shape.m*shape.k, 0),
                    std::vector<std::int8_t>(shape.n*shape.k, 0)};
    std::fill_n(inputs.a.begin(), shape.k, static_cast<std::int8_t>(-128));
    std::fill_n(inputs.bt.begin(), shape.k, static_cast<std::int8_t>(127));
    const auto output = reference_matmul(shape, inputs);
    check(output[0] == static_cast<std::uint32_t>(-66584576), "int8 negative extreme dot product");
    check(std::all_of(output.begin()+1, output.begin()+shape.cells(), [](auto v){return v==0;}), "zero rows and columns");
    for (unsigned slot = 0; slot < 16; ++slot) {
        const auto first = static_cast<std::uint32_t>(-2080768LL * (slot+1));
        const auto later = static_cast<std::uint32_t>(-2080768LL * (slot+17));
        const auto expected = ((first << 13) | (first >> 19)) ^ later;
        check(output[shape.cells()+slot] == expected, "cumulative transcript slot " + std::to_string(slot));
    }
    return checks;
}
} // namespace pearl
