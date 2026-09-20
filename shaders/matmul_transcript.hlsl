// Correctness-first diagnostic, cs_5_1. No model, network job, or proof submitted.
// Contiguous 16x16 output/hash tile. Rank=128; inputs are packed signed bytes.
ByteAddressBuffer A : register(t0);
ByteAddressBuffer BT : register(t1);
RWByteAddressBuffer Output : register(u0);
cbuffer Shape : register(b0) { uint M; uint N; uint K; uint Rank; };

groupshared int TileA[16][16];
groupshared int TileBT[16][16];
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
    for (uint base = 0; base < K; base += 16) {
        // Every thread loads one element of each 16x16 tile. BT rows map to
        // output columns; local.x is the reduction dimension for both loads.
        uint ai = row * K + base + local.x;
        uint bi = (group.x * 16 + local.y) * K + base + local.x;
        TileA[local.y][local.x] = signed_byte(A.Load(ai & ~3u), ai);
        TileBT[local.y][local.x] = signed_byte(BT.Load(bi & ~3u), bi);
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint d = 0; d < 16; ++d)
            sum += TileA[local.y][d] * TileBT[local.x][d];
        GroupMemoryBarrierWithGroupSync();

        if ((base + 16) % Rank == 0) {
            Reduction[tid] = asuint(sum); // cumulative, NOT reset each rank
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
                if (tid < stride) Reduction[tid] ^= Reduction[tid+stride];
                GroupMemoryBarrierWithGroupSync();
            }
            if (tid == 0) {
                uint slot = (((base+16) / Rank) - 1) & 15;
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
