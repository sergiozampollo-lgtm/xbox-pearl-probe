#pragma once
#include "pearl_core.h"

namespace pearl {
using Header = std::array<std::uint8_t,76>;
using Bytes = std::vector<std::uint8_t>;

// Dense V3, rank 128, contiguous 16x16 hash tiles only. No legacy fallback.
std::array<std::uint8_t,52> mining_config(const Shape&);
Hash compute_job_key(const Header&, const Shape&);
Header header_from_hex(const std::string&);
Bytes decode_hex(const std::string&);
std::string encode_hex(const void*, std::size_t);

class MatrixTree {
public:
    MatrixTree(const std::vector<std::int8_t>& raw, const Hash& key);
    const Hash& root() const { return layers_.back().front(); }
    // Canonical bincode 1.3 fixed-int MatrixMerkleProof, including row indices.
    Bytes open_rows(std::uint32_t first_row, std::uint32_t k) const;
private:
    Bytes data_;
    std::vector<std::vector<Hash>> layers_;
};

struct DenseWork {
    Shape shape;
    Hash job_key;
    Matrices raw;
    MatrixTree a_tree, b_tree;
    Seeds seeds;
    Matrices noised;
    DenseWork(const Header&, const Shape&, const Hash& entropy);
    Bytes proof(std::uint32_t tile_row, std::uint32_t tile_col) const;
    Transcript transcript(const std::vector<std::uint32_t>& output, std::size_t tile) const;
};

// Compare the LE jackpot to a BE *base* share target scaled by 256*K.
// Scaling follows Pearl's work normalization; rank=128 has no rank penalty.
bool meets_base_target(const Hash& jackpot_le, const Hash& base_target_be, const Shape&);
} // namespace pearl
