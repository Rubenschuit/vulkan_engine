#include "pch.hpp"
#include "axes_render_system.hpp"
#include "glm/gtc/constants.hpp"

namespace ve {

	struct AxesPushConstants {
		alignas(16) glm::vec3 offset{0.0f};
		alignas(16) glm::vec3 scale{1.0f};
	};

	AxesRenderSystem::AxesRenderSystem(
			VeDevice& device,
			vk::raii::DescriptorSetLayout& global_set_layout,
			vk::Format color_format) : ve_device(device) {
		createPipelineLayout(global_set_layout);
		createPipeline(color_format);
		createAxesModel();
	}

	AxesRenderSystem::~AxesRenderSystem() {}

	void AxesRenderSystem::createPipelineLayout(vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(AxesPushConstants)
	};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 1,
		.pSetLayouts = &*global_set_layout,
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
		// 3 axes as cylinders from origin to +L along each axis, colored RGB
		std::vector<VeModel::Vertex> vertices;
		const int SEGMENTS = 24;           // circle segments per cylinder
		const float L = 100.0f;              // cylinder length
		const float R = 0.003f;            // cylinder radius
		vertices.reserve(3 * SEGMENTS * 6); // 2 tris per quad, 3 axes

		auto push_tri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& color){
			vertices.push_back({a, color, glm::vec3{1.0f}, {0.f, 0.f}});
			vertices.push_back({b, color, glm::vec3{1.0f}, {0.f, 0.f}});
			vertices.push_back({c, color, glm::vec3{1.0f}, {0.f, 0.f}});
		};

		const float two_pi = glm::two_pi<float>();

		// X-axis cylinder (red), rings in the YZ plane
		{
			glm::vec3 col{1.f, 0.f, 0.f};
			for (int i = 0; i < SEGMENTS; ++i) {
				float a0 = two_pi * (static_cast<float>(i) / SEGMENTS);
				float a1 = two_pi * (static_cast<float>(i + 1) / SEGMENTS);
				float y0 = R * std::cos(a0), z0 = R * std::sin(a0);
				float y1 = R * std::cos(a1), z1 = R * std::sin(a1);
				glm::vec3 v0{0.f, y0, z0};
				glm::vec3 v1{L,  y0, z0};
				glm::vec3 v2{L,  y1, z1};
				glm::vec3 v3{0.f, y1, z1};
				push_tri(v0, v1, v2, col);
				push_tri(v2, v3, v0, col);
			}
		}

		// Y-axis cylinder (green), rings in the XZ plane
		{
			glm::vec3 col{0.f, 1.f, 0.f};
			for (int i = 0; i < SEGMENTS; ++i) {
				float a0 = two_pi * (static_cast<float>(i) / SEGMENTS);
				float a1 = two_pi * (static_cast<float>(i + 1) / SEGMENTS);
				float x0 = R * std::cos(a0), z0 = R * std::sin(a0);
				float x1 = R * std::cos(a1), z1 = R * std::sin(a1);
				glm::vec3 v0{x0, 0.f, z0};
				glm::vec3 v1{x0, L,  z0};
				glm::vec3 v2{x1, L,  z1};
				glm::vec3 v3{x1, 0.f, z1};
				push_tri(v0, v1, v2, col);
				push_tri(v2, v3, v0, col);
			}
		}

		// Z-axis cylinder (blue), rings in the XY plane
		{
			glm::vec3 col{0.f, 0.f, 1.f};
			for (int i = 0; i < SEGMENTS; ++i) {
				float a0 = two_pi * (static_cast<float>(i) / SEGMENTS);
				float a1 = two_pi * (static_cast<float>(i + 1) / SEGMENTS);
				float x0 = R * std::cos(a0), y0 = R * std::sin(a0);
				float x1 = R * std::cos(a1), y1 = R * std::sin(a1);
				glm::vec3 v0{x0, y0, 0.f};
				glm::vec3 v1{x0, y0, L };
				glm::vec3 v2{x1, y1, L };
				glm::vec3 v3{x1, y1, 0.f};
				push_tri(v0, v1, v2, col);
				push_tri(v2, v3, v0, col);
			}
		}
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
