#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pearl {
using Hash = std::array<std::uint8_t, 32>;
using Transcript = std::array<std::uint32_t, 16>;

Hash digest(const void* data, std::size_t size, const Hash* key = nullptr);
Hash from_hex(const std::string& value);
std::string to_hex(const Hash& value);

struct Seeds { Hash a; Hash b; };
// Dense V3 only. No default/fallback to legacy certificate rules.
Seeds seeds_v3(const Hash& job_key, const Hash& root_a, const Hash& root_b,
               std::uint32_t m, std::uint32_t n);
Hash jackpot_hash(const Transcript& transcript, const Hash& a_seed);
std::size_t cpu_blake3_simd_degree();

// Deliberately a fixed diagnostic subset, not a full consensus validator.
// Contiguous 16x16 hash tiles, rank 128, no MoE, dimensions multiples of 16.
struct Shape {
    std::uint32_t m, n, k;
    static constexpr std::uint32_t rank = 128;
    void validate_probe() const;
    std::size_t cells() const;
    std::size_t tiles() const;
    std::size_t output_words() const;
};

struct Matrices {
    // Signed bytes; B is already transposed, laid out as n rows of length k.
    // Inputs here are synthetic, already-noised-shaped data, NOT valid jobs.
    std::vector<std::int8_t> a;
    std::vector<std::int8_t> bt;
};
Matrices probe_matrices(const Shape& shape, std::uint32_t seed);
// Layout: m*n final int32 bit patterns, followed by tiles*16 transcript words.
// Cumulative sums are XOR-reduced every rank, then rotl13/XOR into 16 words.
std::vector<std::uint32_t> reference_matmul(const Shape&, const Matrices&);
void transcript_accumulate(Transcript&, std::uint32_t round, std::uint32_t value);

// Includes upstream BLAKE3 known answers and Pearl V3 golden seed vectors.
// Throws with a specific check name on failure. Returns number of checks.
unsigned self_test();
} // namespace pearl
