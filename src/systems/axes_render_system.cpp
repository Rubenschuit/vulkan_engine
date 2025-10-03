#include "pch.hpp"
#include "axes_render_system.hpp"

namespace ve {

	struct AxesPushConstants {
		alignas(16) glm::vec3 offset{0.0f};
		alignas(16) glm::vec3 scale{1.0f};
	};

	AxesRenderSystem::AxesRenderSystem(
			VeDevice& device,
			vk::raii::DescriptorSetLayout& descriptor_set_layout,
			vk::Format color_format) : ve_device(device) {
		createPipelineLayout(descriptor_set_layout);
		createPipeline(color_format);
		createAxesModel();
	}

	AxesRenderSystem::~AxesRenderSystem() {}

	void AxesRenderSystem::createPipelineLayout(vk::raii::DescriptorSetLayout& descriptor_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(AxesPushConstants)
	};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 1,
		.pSetLayouts = &*descriptor_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	pipeline_layout = vk::raii::PipelineLayout(ve_device.getDevice(), pipeline_layout_info);
	}

	void AxesRenderSystem::createPipeline(vk::Format color_format) {
		PipelineConfigInfo cfg{};
		VePipeline::defaultPipelineConfigInfo(cfg);
		// Draw axes as thin triangles (quads) instead of lines so depth bias applies reliably
		cfg.input_assembly_info.topology = vk::PrimitiveTopology::eTriangleList;
		cfg.depth_stencil_info.depthTestEnable = VK_TRUE;
		cfg.depth_stencil_info.depthWriteEnable = VK_FALSE;
		cfg.rasterization_info.depthBiasEnable = VK_TRUE;
		cfg.rasterization_info.depthBiasConstantFactor = 8192.0f;
		cfg.rasterization_info.depthBiasClamp = 0.0f;
		cfg.color_format = color_format;
		cfg.pipeline_layout = pipeline_layout;
		ve_pipeline = std::make_unique<VePipeline>(
			ve_device,
			"../shaders/axes_shader.spv",
			cfg
		);
		assert(ve_pipeline && "Failed to create axes pipeline");
	}

	void AxesRenderSystem::createAxesModel() {
		// 3 axes, from origin to +1 in each axis, colored RGB
		std::vector<VeModel::Vertex> vertices;
		vertices.reserve(6 * 3); // 6 verts per axis * 3 axes

		// Local axis dimensions: length (L) and half-width (W)
		const float L = 10.0f;   // axis length from origin to +L
		const float W = 0.005f;  // half thickness of the quad representing the axis

		auto push_tri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& color){
		vertices.push_back({a, color, {0.f, 0.f}});
		vertices.push_back({b, color, {0.f, 0.f}});
		vertices.push_back({c, color, {0.f, 0.f}});
		};

		// X axis (red): along +X, quad extends in +/-Y
		{
		glm::vec3 col{1.f, 0.f, 0.f};
		glm::vec3 v0{0.f, -W, 0.f};
		glm::vec3 v1{L, -W, 0.f};
		glm::vec3 v2{L,  W, 0.f};
		glm::vec3 v3{0.f,  W, 0.f};
		push_tri(v0, v1, v2, col);
		push_tri(v2, v3, v0, col);
		}

		// Y axis (green): along +Y, quad extends in +/-X
		{
		glm::vec3 col{0.f, 1.f, 0.f};
		glm::vec3 v0{-W, 0.f, 0.f};
		glm::vec3 v1{-W, L, 0.f};
		glm::vec3 v2{ W, L, 0.f};
		glm::vec3 v3{ W, 0.f, 0.f};
		push_tri(v0, v1, v2, col);
		push_tri(v2, v3, v0, col);
		}

		// Z axis (blue): along +Z, quad extends in +/-X
		{
		glm::vec3 col{0.f, 0.f, 1.f};
		glm::vec3 v0{-W, 0.f, 0.f};
		glm::vec3 v1{-W, 0.f, L};
		glm::vec3 v2{ W, 0.f, L};
		glm::vec3 v3{ W, 0.f, 0.f};
		push_tri(v0, v1, v2, col);
		push_tri(v2, v3, v0, col);
		}

		// Non-indexed triangles
		axes_model = std::make_unique<VeModel>(ve_device, vertices);
	}

	void AxesRenderSystem::renderAxes(VeFrameInfo& frame_info) const {
		frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, ve_pipeline->getPipeline());
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*pipeline_layout,
			0,
			*frame_info.global_descriptor_set,
			{}
		);

		AxesPushConstants push{}; // origin, unit scale
		frame_info.command_buffer.pushConstants<AxesPushConstants>(
			*pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			push
		);

		axes_model->bindVertexBuffer(frame_info.command_buffer);
		axes_model->draw(frame_info.command_buffer);
	}

}
