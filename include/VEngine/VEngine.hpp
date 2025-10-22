#pragma once

// Included by application

#include "ve_export.hpp" // for VENGINE_API

#include "core/ve_window.hpp"
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
#include "core/ve_buffer.hpp"
#include "core/ve_image.hpp"
#include "core/ve_compute_pipeline.hpp"
#include "core/ve_descriptors.hpp"
#include "core/ve_swap_chain.hpp"

#include "core/ve_renderer.hpp"
#include "core/ve_texture.hpp"

#include "game/ve_frame_info.hpp"
#include "game/ve_game_object.hpp"
#include "game/ve_camera.hpp"
#include "game/ve_model.hpp"

#include "utils/ve_log.hpp"
#include "input/input_controller.hpp"

#include "ui/imgui_layer.hpp"

#include "systems/simple_render_system.hpp"
#include "systems/axes_render_system.hpp"
#include "systems/point_light_system.hpp"
#include "systems/particle_system.hpp"
#include "systems/skybox_render_system.hpp"