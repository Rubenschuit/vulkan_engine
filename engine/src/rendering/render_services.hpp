#pragma once
#include "ve_export.hpp"

namespace ve {

class ParticleBackend;
class ShadowRenderSystem;
class SkyboxRenderSystem;

// Bundle of editor- and app-facing render systems owned by RenderPipeline.

struct VENGINE_API RenderServices {
	SkyboxRenderSystem* skybox    = nullptr;
	ShadowRenderSystem* shadow    = nullptr;
	ParticleBackend*    particles = nullptr;
};

}