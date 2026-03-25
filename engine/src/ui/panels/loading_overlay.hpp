#pragma once
#include "ve_export.hpp"
#include <string>

namespace ve {

class AssetLoadingSystem;

class VENGINE_API LoadingPanel {
public:
	void render(AssetLoadingSystem& loader);
};

} // namespace ve
