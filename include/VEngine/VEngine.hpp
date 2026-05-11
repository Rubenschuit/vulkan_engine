// Included by application using VEngine
#pragma once

#include "application/ve_application.hpp"
#include "application/ve_engine_config.hpp"

#include "ve_export.hpp" // for VENGINE_API

#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_swap_chain.hpp"

#include "rendering/ve_renderer.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_resource_manager.hpp"

#include "rendering/ve_frame_info.hpp"
#include "scene/ve_component.hpp"
#include "scene/camera_view.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_scene.hpp"
#include "scene/gltf_scene.hpp"

#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#include "utils/ve_log.hpp"
#include "input/input_controller.hpp"
#include "input/input_action.hpp"

#include "ui/imgui_layer.hpp"

#include "rendering/culling/culling_system.hpp"
#include "rendering/aabb_debug_render_system.hpp"
#include "rendering/axes_render_system.hpp"
#include "rendering/light_system.hpp"
#include "rendering/particle_system.hpp"
#include "rendering/skybox_render_system.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/fireworks_system.hpp"
#include "rendering/bloom_system.hpp"
#include "rendering/post_process_system.hpp"
#include "rendering/cluster_light_system.hpp"
#include "rendering/gtao_system.hpp"
#include "rendering/shadow_mask_system.hpp"

#include "resources/ve_material_properties.hpp"
#include "utils/ve_random.hpp"
#include "ui/editor.hpp"