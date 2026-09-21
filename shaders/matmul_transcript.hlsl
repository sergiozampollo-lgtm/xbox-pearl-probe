// Exact integer compute, cs_5_1. The host owns proof validation and networking.
// Contiguous 16x16 output/hash tile. Rank=128; inputs are packed signed bytes.
ByteAddressBuffer A : register(t0);
ByteAddressBuffer BT : register(t1);
RWByteAddressBuffer Output : register(u0);
cbuffer Shape : register(b0) { uint M; uint N; uint K; uint Rank; };

// Stage 64 K elements at once: each lane loads four signed bytes with one
// aligned load. This reduces workgroup barriers while retaining every rank
// checkpoint. The odd LDS row stride avoids the repeated bank mapping.
// https://gpuopen.com/learn/rdna-performance-guide/#compute-shaders
static const uint KTile = 64;
groupshared int TileA[16][KTile+1];
groupshared int TileBT[16][KTile+1];
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
        [unroll] for (uint j = 0; j < 4; ++j) {
            TileA[local.y][local.x*4+j] = signed_byte(a4,j);
            TileBT[local.y][local.x*4+j] = signed_byte(b4,j);
        }
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint d = 0; d < KTile; ++d)
            sum += TileA[local.y][d] * TileBT[local.x][d];
        GroupMemoryBarrierWithGroupSync();

        if ((base + KTile) % Rank == 0) {
            Reduction[tid] = asuint(sum); // cumulative, NOT reset each rank
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
                if (tid < stride) Reduction[tid] ^= Reduction[tid+stride];
                GroupMemoryBarrierWithGroupSync();
            }
            if (tid == 0) {
                uint slot = (((base+KTile) / Rank) - 1) & 15;
                uint old = Transcript[slot];
                Transcript[slot] = ((old << 13) | (old >> 19)) ^ Reduction[0];
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    Output.Store((row*N + col)*4, asuint(sum));
    if (tid < 16) {
        uint tile = group.y*(N/16) + group.x;
        Output.Store((M*N + tile*16 + tid)*4, Transcript[tid]);
    }
}
