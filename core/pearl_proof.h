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
    // Leaves and parents are hashed in batches with blake3_hash_many (SIMD lanes);
    // chunk counters, CHUNK_START/CHUNK_END/PARENT/ROOT flags and keyed mode are
    // the official BLAKE3 tree rules, so root() equals the keyed BLAKE3 digest.
    MatrixTree(const std::vector<std::int8_t>& raw, const Hash& key);
    const Hash& root() const { return layers_.back().front(); }
    // Independent check against the official hasher (full pass over the data).
    bool verify_root() const;
    // Replace one 1 KiB chunk and rehash only its root path (leaf + parents).
    void update_chunk(std::size_t index, const std::int8_t* chunk);
    std::size_t chunks() const { return layers_.front().size(); }
    // Canonical bincode 1.3 fixed-int MatrixMerkleProof, including row indices.
    Bytes open_rows(std::uint32_t first_row, std::uint32_t k) const;
private:
    void hash_leaves(std::size_t first, std::size_t count);
    void hash_parents(std::size_t layer, std::size_t first_pair, std::size_t pairs);
    Hash key_;
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
    // verify_roots: also recompute both roots with the official hasher (diagnostic /
    // sampled integrity check; the tree construction itself is exact without it).
    DenseWork(const Header&, const Shape&, const Hash& entropy, bool verify_roots = true);
    Bytes proof(std::uint32_t tile_row, std::uint32_t tile_col) const;
    // Accept full diagnostic output or compact production transcripts.
    Transcript transcript(const std::vector<std::uint32_t>& output, std::size_t tile) const;
};

// Compare the LE jackpot to a BE *base* share target scaled by 256*K.
// Scaling follows Pearl's work normalization; rank=128 has no rank penalty.
bool meets_base_target(const Hash& jackpot_le, const Hash& base_target_be, const Shape&);
} // namespace pearl
