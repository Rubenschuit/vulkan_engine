/*
 * CPU mirrors of rc_world_common.slangh's constants, in that file's order.
 */

#pragma once
#include <cstdint>

namespace ve::rcw {

// RcwPush.flags layout. bit 7 and bits 26-31 free.
inline constexpr uint32_t FLAG_ROUGH_SPEC = 1u << 0;
inline constexpr uint32_t FLAG_VISIBILITY_EVIDENCE = 1u << 1;
inline constexpr uint32_t FLAG_HIST_DEPTH = 1u << 2;
inline constexpr uint32_t FLAG_COUNT_WEIGHT = 1u << 3;
inline constexpr uint32_t FLAG_FREEZE_PROBES = 1u << 24;
inline constexpr uint32_t FLAG_SINGLE_FRAME = 1u << 25;
inline constexpr uint32_t PARENT_MODE_SHIFT = 4;        // bit 4
inline constexpr uint32_t DEPOSIT_MODE_SHIFT = 5;       // bits 5-6
inline constexpr uint32_t SKY_MIN_FRAC_SHIFT = 8;       // bits 8-15
inline constexpr uint32_t SKY_TRUST_SHIFT = 16;         // bits 16-23

// Store layout
inline constexpr uint32_t MAX_CASCADES = 6;
inline constexpr uint32_t LINEAR_WG = 64;
inline constexpr uint32_t SCRATCH_WORDS = 5;
inline constexpr uint32_t TILE = 8;

// Ray exit codes 
inline constexpr uint32_t RAY_STAT_WORDS = 8;
inline constexpr uint32_t RAY_HIT = 0;
inline constexpr uint32_t RAY_CLEAR = 1;
inline constexpr uint32_t RAY_SKY = 2;
inline constexpr uint32_t RAY_SKY_GATED = 3;
inline constexpr uint32_t RAY_UNK_OCCLUDED = 4;
inline constexpr uint32_t RAY_UNK_STEPCAP = 5;
inline constexpr uint32_t RAY_UNK_EDGE = 6;
inline constexpr uint32_t RAY_UNK_CLIP = 7;

// Mirrors RcwCounters. The x/y/ztriples are VkDispatchIndirectCommand workgroup counts.
struct CountersGpu {
	uint32_t probes;
	uint32_t overflow;
	uint32_t linear_x, linear_y, linear_z;
	uint32_t pair_x, pair_y, pair_z;
	uint32_t probe_x, probe_y, probe_z;
	uint32_t husks;
};
static_assert(sizeof(CountersGpu) == 12 * sizeof(uint32_t));

// Specialization constant ids ([vk::constant_id] in the shader).
inline constexpr uint32_t THETA0_SPEC_ID = 7;
inline constexpr uint32_t C0_RAY_LENGTH_SPEC_ID = 8;

}
