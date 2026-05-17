#include "pch.hpp"
#include "rendering/skinning_pre_pass.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "scene/ecs_event_dispatcher.hpp"
#include "resources/ve_mesh.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "utils/ve_log.hpp"

namespace ve {

namespace {
	constexpr uint32_t MAX_PALETTE_MATRICES = 4096;
	constexpr uint32_t WORKGROUP_SIZE = 64;
}

struct SkinPushConstant {
	uint32_t vertex_count;
	uint32_t palette_offset;
};

SkinningPrePass::SkinningPrePass(VeDevice& device, VeDescriptorPool& descriptor_pool,
                                 std::filesystem::path shader_path, EventBus& event_bus)
	: m_ve_device(device), m_descriptor_pool(descriptor_pool),
	  m_shader_path(std::move(shader_path)) {

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_palette_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(glm::mat4),
			MAX_PALETTE_MATRICES,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_palette_ssbos[i]->map();
	}

	createSetLayout();
	createPipelineLayout();
	createPipeline();

	event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		subscribeToRegistry(*e.registry);
	});
	event_bus.subscribe<SceneUnloadedEvent>([this](const SceneUnloadedEvent&) {
		// Registry's event dispatcher dies with the scene
		m_instance_outputs.clear();
		m_graveyard.clear();
	});
}

SkinningPrePass::~SkinningPrePass() = default;

void SkinningPrePass::createSetLayout() {
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // input vertices
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // input skin attribs
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // palette
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // output full vertex
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // output position
		.build();
}

void SkinningPrePass::createPipelineLayout() {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(SkinPushConstant),
	};
	std::array<vk::DescriptorSetLayout, 1> set_layouts{*m_set_layout->getDescriptorSetLayout()};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void SkinningPrePass::createPipeline() {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path, m_pipeline_layout);
}

void SkinningPrePass::subscribeToRegistry(Registry& registry) {
	registry.events().subscribe<EntityDestroyedEvent>(
		[this](const EntityDestroyedEvent& e) { freeEntityOutputs(e.entity); });
	registry.events().subscribe<ComponentRemovedEvent<SkinComponent>>(
		[this](const ComponentRemovedEvent<SkinComponent>& e) { freeEntityOutputs(e.entity); });
}

void SkinningPrePass::freeEntityOutputs(Entity entity) {
	const uint64_t hi = static_cast<uint64_t>(entity.id()) << 32;
	const uint64_t release_frame = m_frame_counter + MAX_FRAMES_IN_FLIGHT;
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		auto it = m_instance_outputs.find(hi | f);
		if (it == m_instance_outputs.end())
			continue;
		m_graveyard.push_back({release_frame, std::move(it->second)});
		m_instance_outputs.erase(it);
	}
}

void SkinningPrePass::updatePalette(Registry& registry, uint32_t frame_index) {
	// Handle graveyard entries
	++m_frame_counter;
	while (!m_graveyard.empty() && m_graveyard.front().release_frame <= m_frame_counter)
		m_graveyard.pop_front();

	auto& dispatches = m_pending_dispatches[frame_index];
	dispatches.clear();

	auto* palette_ptr = static_cast<glm::mat4*>(m_palette_ssbos[frame_index]->getMappedMemory());
	uint32_t palette_cursor = 0;

	bool overflow_warned_this_frame = false;
	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>()) {
		VeMesh* mesh = mc.getMesh();
		if (!mesh || !mesh->hasSkinning())
			continue;
		const auto& joints = sc.getJointEntities();
		const auto& ibms = sc.getInverseBindMatrices();
		if (joints.empty() || joints.size() != ibms.size())
			continue;

		uint32_t joint_count = static_cast<uint32_t>(joints.size());
		if (palette_cursor + joint_count > MAX_PALETTE_MATRICES) {
			if (!overflow_warned_this_frame) {
				VE_LOGW("SkinningPrePass: palette capacity exceeded (cap="
				        << MAX_PALETTE_MATRICES << "); some skinned meshes will be skipped this frame");
				overflow_warned_this_frame = true;
			}
			continue;
		}

		// Mark the mesh's world AABB stale once per frame so downstream getWorldAABB() calls
		// recompute exactly once
		mc.invalidateWorldAABB();

		const glm::mat4 inv_mesh_world = glm::inverse(registry.getWorldTransform(entity));
		uint32_t base = palette_cursor;
		for (uint32_t j = 0; j < joint_count; j++) {
			Entity je = joints[j];
			glm::mat4 joint_world = (!je.isNull() && registry.isAlive(je))
				? registry.getWorldTransform(je) : glm::mat4(1.0f);
			palette_ptr[base + j] = inv_mesh_world * joint_world * ibms[j];
		}
		palette_cursor += joint_count;
		sc.setPaletteOffset(base);

		dispatches.push_back({
			.entity = entity,
			.vertex_count = mesh->getVertexCount(),
			.palette_offset = base,
		});
	}

	m_palette_count[frame_index] = palette_cursor;
}

SkinningPrePass::InstanceFrame& SkinningPrePass::getOrAllocateOutputs(
		Entity entity, uint32_t frame_index, uint32_t vertex_count,
		VeBuffer& input_vertex_buf, VeBuffer& input_skin_buf) {

	uint64_t key = makeKey(entity, frame_index);
	auto& slot = m_instance_outputs[key];

	VkBuffer new_vbo = input_vertex_buf.getBuffer();
	VkBuffer new_skin = input_skin_buf.getBuffer();
	bool needs_realloc = !slot.out_full
		|| slot.vertex_count != vertex_count
		|| slot.input_vertex_buf != new_vbo
		|| slot.input_skin_buf != new_skin;
	if (needs_realloc) {
		slot.out_full = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(VeMesh::Vertex),
			vertex_count,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eVertexBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal);
		slot.out_pos = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(glm::vec3),
			vertex_count,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eVertexBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal);
		slot.vertex_count = vertex_count;
		slot.input_vertex_buf = new_vbo;
		slot.input_skin_buf = new_skin;
		writeDescriptor(slot, frame_index, input_vertex_buf, input_skin_buf);
	}

	return slot;
}

void SkinningPrePass::writeDescriptor(InstanceFrame& inst, uint32_t frame_index,
                                      VeBuffer& input_vertex_buf, VeBuffer& input_skin_buf) {
	auto vbo_info = input_vertex_buf.getDescriptorInfo();
	auto skin_info = input_skin_buf.getDescriptorInfo();
	auto palette_info = m_palette_ssbos[frame_index]->getDescriptorInfo();
	auto out_full_info = inst.out_full->getDescriptorInfo();
	auto out_pos_info = inst.out_pos->getDescriptorInfo();

	VeDescriptorWriter(*m_set_layout, m_descriptor_pool)
		.writeBuffer(0, &vbo_info)
		.writeBuffer(1, &skin_info)
		.writeBuffer(2, &palette_info)
		.writeBuffer(3, &out_full_info)
		.writeBuffer(4, &out_pos_info)
		.build(inst.descriptor_set);
}

void SkinningPrePass::dispatch(VeFrameInfo& fi) {
	uint32_t frame = fi.current_frame;
	const auto& dispatches = m_pending_dispatches[frame];
	if (dispatches.empty())
		return;

	auto& cmd = fi.compute_command_buffer;
	auto& registry = *fi.registry;

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());

	for (const SkinDispatch& d : dispatches) {
		Entity entity = d.entity;
		if (!registry.isAlive(entity))
			continue;
		auto* mc = registry.getComponent<MeshComponent>(entity);
		if (!mc)
			continue;
		VeMesh* mesh = mc->getMesh();
		if (!mesh || !mesh->hasSkinning())
			continue;

		auto& inst = getOrAllocateOutputs(entity, frame, d.vertex_count,
		                                  mesh->getVertexBuffer(), mesh->getSkinVertexBuffer());

		std::array<vk::DescriptorSet, 1> sets{*inst.descriptor_set};
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, sets, {});

		SkinPushConstant pc{
			.vertex_count = d.vertex_count,
			.palette_offset = d.palette_offset,
		};
		cmd.pushConstants<SkinPushConstant>(*m_pipeline_layout,
			vk::ShaderStageFlagBits::eCompute, 0, pc);

		uint32_t groups = (d.vertex_count + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
		cmd.dispatch(groups, 1, 1);
	}

	// Make compute writes visible to subsequent vertex-input reads.
	vk::MemoryBarrier2 compute_done{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput,
		.dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead,
	};
	vk::DependencyInfo dep{
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &compute_done,
	};
	cmd.pipelineBarrier2(dep);
}

VeBuffer* SkinningPrePass::getOutputFullBuffer(Entity entity, uint32_t frame_index) const {
	auto it = m_instance_outputs.find(makeKey(entity, frame_index));
	return (it != m_instance_outputs.end()) ? it->second.out_full.get() : nullptr;
}

VeBuffer* SkinningPrePass::getOutputPositionBuffer(Entity entity, uint32_t frame_index) const {
	auto it = m_instance_outputs.find(makeKey(entity, frame_index));
	return (it != m_instance_outputs.end()) ? it->second.out_pos.get() : nullptr;
}

} // namespace ve