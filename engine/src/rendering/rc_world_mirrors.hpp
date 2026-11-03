/*
 * CPU mirrors of rc_world_common.slangh's constants, in that file's order.
 */

#pragma once
#include "resources/ve_mesh.hpp"

#include <cstddef>
#include <cstdint>

namespace ve::rcw {

// RcwPush.flags layout, in rc_world_common.slangh's order. Bit 0 and bits 4-31 free.
inline constexpr uint32_t FLAG_VISIBILITY_EVIDENCE = 1u << 1;
inline constexpr uint32_t FLAG_HIST_DEPTH = 1u << 2;
inline constexpr uint32_t FLAG_SINGLE_FRAME = 1u << 3;

// Store layout
inline constexpr uint32_t MAX_CASCADES = 6;
inline constexpr uint32_t LINEAR_WG = 64;
inline constexpr uint32_t SCRATCH_RADIANCE_WORDS = 3;
inline constexpr uint32_t TILE = 8;

// Ray exit codes
inline constexpr uint32_t RAY_HIT = 0;
inline constexpr uint32_t RAY_CLEAR = 1;
inline constexpr uint32_t RAY_SKY = 2;
inline constexpr uint32_t RAY_UNKNOWN = 3;
inline constexpr uint32_t RAY_EXIT_CODES = 4;

// Mirrors RcwCounters. The x/y/ztriples are VkDispatchIndirectCommand workgroup counts.
struct CountersGpu {
	uint32_t probes;
	uint32_t overflow;
	uint32_t linear_x, linear_y, linear_z;
	uint32_t pair_x, pair_y, pair_z;
	uint32_t probe_x, probe_y, probe_z;
	uint32_t husks;
	uint32_t raygen_cursor;
	uint32_t rays[RAY_EXIT_CODES]; // cascade 0 only
};
static_assert(sizeof(CountersGpu) == 17 * sizeof(uint32_t));
static_assert(sizeof(VeMesh::Vertex) == 64 && offsetof(VeMesh::Vertex, pos) == 0
	&& offsetof(VeMesh::Vertex, tex_coord) == 24 && offsetof(VeMesh::Vertex, color) == 48,
	"RCW_VERTEX_* in rc_world_common.slangh");

// RCW_THETA0: polar bins of the cascade-0 direction grid, which sizes the store.
inline constexpr uint32_t THETA0 = 4;

}
