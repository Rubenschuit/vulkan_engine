#pragma once
// AMD FidelityFX SPD dispatch setup shared by the depth-pyramid builders
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace ve {

struct SpdConstants {
	uint32_t   mips;
	uint32_t   numWorkGroups;
	glm::vec2  invInputSize;
	glm::vec2  workGroupOffset;
	glm::uvec2 srcSize;
	glm::uvec2 mip5Extent;
};

inline uint32_t nextPow2(uint32_t v) {
	if (v <= 1)
		return 1;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v + 1;
}

// Mirror of SPD's SpdSetup() helper. Returns dispatch thread group counts and
// the number of mips to write. We always downsample the full image starting at
// (0, 0), so workGroupOffset is implicitly 0.
struct SpdDispatch {
	uint32_t group_count_x;
	uint32_t group_count_y;
	uint32_t num_work_groups;
	uint32_t mip_count;
};

inline SpdDispatch spdSetup(uint32_t src_w, uint32_t src_h, uint32_t max_mips) {
	SpdDispatch out{};
	out.group_count_x = (src_w + 63u) / 64u;
	out.group_count_y = (src_h + 63u) / 64u;
	out.num_work_groups = out.group_count_x * out.group_count_y;
	uint32_t resolution = std::max(src_w, src_h);
	uint32_t derived = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(resolution))));
	out.mip_count = std::min(derived, max_mips);
	return out;
}

}