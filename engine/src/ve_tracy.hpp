#pragma once

#ifdef TRACY_ENABLE
	#include <tracy/Tracy.hpp>
	#include <tracy/TracyVulkan.hpp>
#else
	#define ZoneScoped
	#define ZoneScopedN(x)
	#define FrameMark

	#define TracyVkContext(a, b, c, d, e, f, g) nullptr
	#define TracyVkContextCalibrated(a, b, c, d, e, f, g) nullptr
	#define TracyVkDestroy(x)
	#define TracyVkCollect(ctx, cmd)
	#define TracyVkZone(ctx, cmd, name)

	using TracyVkCtx = void*;
#endif
