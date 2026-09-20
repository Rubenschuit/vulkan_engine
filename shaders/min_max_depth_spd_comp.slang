// SSR min/max depth pyramid generation via AMD FidelityFX SPD.
// Two channels:
// .x = per-2x2 MIN (farthest under reverse-Z), .y = MAX (closest).
//
// Padding reads return (0, 0) = empty/far
//
// Dispatch: ((width + 63) / 64, (height + 63) / 64, 1).

#define A_GPU 1
#define A_HLSL 1
#define SPD_NO_WAVE_OPERATIONS 1

#include "ffx_a.h"

[[vk::binding(0, 0)]] Texture2D<float>   src_image;
[[vk::binding(1, 0)]] SamplerState       point_sampler;

// One descriptor per mip level
[[vk::binding(2, 0)]] globallycoherent RWTexture2D<float2> imgDst[12];

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
	// Reads past the real source extent return (0, 0)
	if (p.x >= int(spdConstants.srcSize.x) || p.y >= int(spdConstants.srcSize.y))
		return AF4(0.0, 0.0, 0.0, 0.0);
	AF2 uv = (AF2(p) + AF2(0.5, 0.5)) * spdConstants.invInputSize;
	float d = src_image.SampleLevel(point_sampler, uv, 0.0);
	return AF4(d, d, 0.0, 0.0);
}

AF4 SpdLoad(ASU2 p, AU1 slice) {
	// The "last workgroup" (which runs mips 6+) reads mip 5 at coords spanning
	// [0, 64) x [0, 64) regardless of mip 5's actual extent. Return (0, 0) for
	// reads beyond the written region, matching the source padding policy.
	if (uint(p.x) >= spdConstants.mip5Extent.x || uint(p.y) >= spdConstants.mip5Extent.y)
		return AF4(0.0, 0.0, 0.0, 0.0);
	float2 v = imgDst[5][p];
	return AF4(v.x, v.y, 0.0, 0.0);
}

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice) {
	imgDst[mip][p] = value.xy;
}

AF4 SpdLoadIntermediate(AU1 x, AU1 y) {
	return spdIntermediate[x][y];
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value) {
	spdIntermediate[x][y] = value;
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3) {
	return AF4(
		min(min(v0.x, v1.x), min(v2.x, v3.x)),
		max(max(v0.y, v1.y), max(v2.y, v3.y)),
		0.0, 0.0);
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