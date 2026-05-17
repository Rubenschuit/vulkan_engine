/* Engine-internal render events.
 *
 * These events carry Vulkan resource handles in their payloads and are
 * therefore not part of the engine's public app surface. Application code
 * should never need to subscribe to or emit these. Subscribers live in
 * engine/src/rendering/ 
 */
#pragma once
#include "events/event_bus.hpp"
#include "rendering/ve_frame_info.hpp"

#include <cstdint>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class VeDescriptorPool;

struct ShadowMaskResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	vk::Extent2D mask_extent;
	vk::Extent2D depth_extent;
	const vk::raii::ImageView& depth_image_view;
	vk::Image depth_image;
};

struct GtaoResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	vk::Extent2D ao_extent;
	vk::Extent2D depth_extent;
	const vk::raii::ImageView& depth_image_view;
	vk::Image depth_image;
};

struct ShadowAtlasResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	ShadowResolutionPreset preset;
};

struct PipelineRecreateEvent {
	vk::Format offscreen_format;
	vk::SampleCountFlagBits sample_count;
};

struct ResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	vk::Extent2D extent;
	vk::Format swap_chain_format;
	vk::Format offscreen_format;
	const vk::raii::ImageView& resolve_target_view;
	const vk::raii::ImageView& depth_image_view;
	vk::Image depth_image;
	const vk::raii::ImageView& wboit_accum_view;
	const vk::raii::ImageView& wboit_revealage_view;
	bool shadow_mask_half_res;
	bool gtao_half_res;
};

}