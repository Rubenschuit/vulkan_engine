#pragma once
#include "ve_export.hpp"

namespace ve {

enum class AlphaMode : uint32_t {
	ALPHA_OPAQUE = 0, // OPAQUE is defined by some windows headers...
	MASK = 1,
	BLEND = 2,
};

struct MaterialAlphaProps {
	AlphaMode alpha_mode = AlphaMode::ALPHA_OPAQUE;
	float alpha_cutoff = 0.5f;
	bool double_sided = false;
};

} // namespace ve
