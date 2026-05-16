/* EditorContext bundles every engine reference the editor and its panels need.
 *
 * VeApplication populates it once after construction and hands it to the Editor
 * via setContext(). The editor distributes the relevant pointers to its panels
 */
#pragma once
#include "ve_export.hpp"

namespace ve {

class AssetLoadingSystem;
class EventBus;
class InputController;
class PhysicsSystem;
class SceneManager;
class ShadowRenderSystem;
class SkyboxRenderSystem;
struct CameraView;

struct VENGINE_API EditorContext {
	SceneManager*          scene_manager     = nullptr;
	SkyboxRenderSystem*    skybox            = nullptr;
	ShadowRenderSystem*    shadow            = nullptr;
	PhysicsSystem*         physics           = nullptr;
	const CameraView*      camera_view       = nullptr;
	const InputController* input_controller  = nullptr;
	EventBus*              event_bus         = nullptr;
};

} // namespace ve