// Hi-Z pyramid generation via AMD FidelityFX SPD (Single-Pass Downsampler).
// Whole pyramid in one dispatch; SPD coordinates inter-mip sync with a single
// global atomic counter.
//
// Reverse-Z: SpdReduce4 returns the per-2x2 MIN to keep the farthest depth.
//
// Dispatch: ((width + 63) / 64, (height + 63) / 64, 1).

#define A_GPU 1
#define A_HLSL 1
#define SPD_NO_WAVE_OPERATIONS 1

#include "ffx_a.h"

[[vk::binding(0, 0)]] Texture2D<float>   src_image;
[[vk::binding(1, 0)]] SamplerState       point_sampler;

// One descriptor per mip level
[[vk::binding(2, 0)]] globallycoherent RWTexture2D<float> imgDst[12];

struct SpdGlobalAtomicBuffer { uint counter; };
[[vk::binding(3, 0)]] globallycoherent RWStructuredBuffer<SpdGlobalAtomicBuffer> spdGlobalAtomic;

struct SpdConstants {
	uint   mips;
	uint   numWorkGroups;
	float2 invInputSize;
	float2 workGroupOffset;
	uint2  srcSize;     // exact source extent (unpadded)
	uint2  mip5Extent;  // valid extent of destination mip 5 (= work group count XY)
};
[[vk::push_constant]] SpdConstants spdConstants;

groupshared AF4 spdIntermediate[16][16];
groupshared AU1 spdCounter;

// --- SPD callback implementations ---

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice) {
	// SPD reads up to padded_source pixels (power of 2). Reads past the
	// real source extent return 1.0, the near plane in reverse-Z, to be
	// conservative and avoid culling visible objects.
	if (p.x >= int(spdConstants.srcSize.x) || p.y >= int(spdConstants.srcSize.y))
		return AF4(1.0, 0.0, 0.0, 0.0);
	AF2 uv = (AF2(p) + AF2(0.5, 0.5)) * spdConstants.invInputSize;
	return AF4(src_image.SampleLevel(point_sampler, uv, 0.0), 0.0, 0.0, 0.0);
}

AF4 SpdLoad(ASU2 p, AU1 slice) {
	// The "last workgroup" (which runs mips 6+) reads mip 5 at coords spanning
	// [0, 64) x [0, 64) regardless of mip 5's actual extent. For sources below
	// 4096 the read range exceeds the written region and corrupt the MIN
	// reduction. Return 1.0 for these out-of-bounds reads.
	if (uint(p.x) >= spdConstants.mip5Extent.x || uint(p.y) >= spdConstants.mip5Extent.y)
		return AF4(1.0, 0.0, 0.0, 0.0);
	return AF4(imgDst[5][p], 0.0, 0.0, 0.0);
}

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice) {
	imgDst[mip][p] = value.x;
}

AF4 SpdLoadIntermediate(AU1 x, AU1 y) {
	return spdIntermediate[x][y];
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value) {
	spdIntermediate[x][y] = value;
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3) {
	return AF4(min(min(v0.x, v1.x), min(v2.x, v3.x)), 0.0, 0.0, 0.0);
}

void SpdIncreaseAtomicCounter(AU1 slice) {
	InterlockedAdd(spdGlobalAtomic[0].counter, 1, spdCounter);
}

AU1 SpdGetAtomicCounter() {
	return spdCounter;
}

void SpdResetAtomicCounter(AU1 slice) {
	spdGlobalAtomic[0].counter = 0;
}

#include "ffx_spd.h"

[numthreads(256, 1, 1)]
void compMain(uint3 wid : SV_GroupID, uint lid : SV_GroupIndex) {
	SpdDownsample(
		AU2(wid.xy),
		AU1(lid),
		AU1(spdConstants.mips),
		AU1(spdConstants.numWorkGroups),
		AU1(wid.z));
}
