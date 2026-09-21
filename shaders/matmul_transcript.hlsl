// Exact integer compute. Default remains the reviewed cs_5_1 implementation.
// Contiguous 16x16 output/hash tile. Rank=128; inputs are packed signed bytes.
#ifndef PEARL_PACKED
#define PEARL_PACKED 0
#endif
#ifndef PEARL_DOT4
#define PEARL_DOT4 0
#endif
#ifndef PEARL_WAVE
#define PEARL_WAVE 0
#endif
#if PEARL_DOT4 && !PEARL_PACKED
#error Packed dot products require packed LDS inputs.
#endif
ByteAddressBuffer A : register(t0);
ByteAddressBuffer BT : register(t1);
RWByteAddressBuffer Output : register(u0);
cbuffer Shape : register(b0) { uint M; uint N; uint K; uint Rank; uint WriteProducts; };

// Stage 64 K elements at once: each lane loads four signed bytes with one
// aligned load. This reduces workgroup barriers while retaining every rank
// checkpoint. The odd LDS row stride avoids the repeated bank mapping.
// https://gpuopen.com/learn/rdna-performance-guide/#compute-shaders
static const uint KTile = 64;
#if PEARL_PACKED
groupshared uint TileA[16][KTile/4+1];
groupshared uint TileBT[16][KTile/4+1];
#else
groupshared int TileA[16][KTile+1];
groupshared int TileBT[16][KTile+1];
#endif
groupshared uint Reduction[256];
groupshared uint Transcript[16];

int signed_byte(uint word, uint offset) {
    uint b = (word >> ((offset & 3) * 8)) & 255;
    return int(b) - (b >= 128 ? 256 : 0);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID,
            uint tid : SV_GroupIndex) {
    uint row = group.y * 16 + local.y;
    uint col = group.x * 16 + local.x;
    if (tid < 16) Transcript[tid] = 0;
    GroupMemoryBarrierWithGroupSync();
    int sum = 0;
    for (uint base = 0; base < K; base += KTile) {
        uint ai = row * K + base + local.x*4;
        uint bi = (group.x * 16 + local.y) * K + base + local.x*4;
        uint a4 = A.Load(ai);
        uint b4 = BT.Load(bi);
#if PEARL_PACKED
        TileA[local.y][local.x] = a4;
        TileBT[local.y][local.x] = b4;
#else
        [unroll] for (uint j = 0; j < 4; ++j) {
            TileA[local.y][local.x*4+j] = signed_byte(a4,j);
            TileBT[local.y][local.x*4+j] = signed_byte(b4,j);
        }
#endif
        GroupMemoryBarrierWithGroupSync();
#if PEARL_PACKED
        [unroll] for (uint d = 0; d < KTile/4; ++d) {
            uint av = TileA[local.y][d];
            uint bv = TileBT[local.x][d];
#if PEARL_DOT4
            sum = dot4add_i8packed(av,bv,sum);
#else
            // A/B control: identical packed layout and loads, scalar arithmetic.
            [unroll] for (uint j = 0; j < 4; ++j)
                sum += signed_byte(av,j) * signed_byte(bv,j);
#endif
        }
#else
        [unroll] for (uint d = 0; d < KTile; ++d)
            sum += TileA[local.y][d] * TileBT[local.x][d];
#endif
        GroupMemoryBarrierWithGroupSync();

        if ((base + KTile) % Rank == 0) {
#if PEARL_WAVE
            // All group threads reach this uniform branch. One atomic XOR per
            // wave avoids assuming any SV_GroupIndex-to-lane mapping or width.
            if (tid == 0) Reduction[0] = 0;
            GroupMemoryBarrierWithGroupSync();
            uint wave_xor = WaveActiveBitXor(asuint(sum));
            if (WaveIsFirstLane()) InterlockedXor(Reduction[0],wave_xor);
            GroupMemoryBarrierWithGroupSync();
#else
            Reduction[tid] = asuint(sum); // cumulative, NOT reset each rank
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
                if (tid < stride) Reduction[tid] ^= Reduction[tid+stride];
                GroupMemoryBarrierWithGroupSync();
            }
#endif
            if (tid == 0) {
                uint slot = (((base+KTile) / Rank) - 1) & 15;
                uint old = Transcript[slot];
                Transcript[slot] = ((old << 13) | (old >> 19)) ^ Reduction[0];
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (WriteProducts != 0) Output.Store((row*N + col)*4, asuint(sum));
    if (tid < 16) {
        uint tile = group.y*(N/16) + group.x;
        uint offset = WriteProducts != 0 ? M*N : 0;
        Output.Store((offset + tile*16 + tid)*4, Transcript[tid]);
    }
}
