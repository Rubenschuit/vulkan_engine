#include "vulkan/ve_descriptors.hpp"

#include <algorithm>
#include <cassert>

namespace ve {

// *************** Descriptor Set Layout Builder *********************

VeDescriptorSetLayout::Builder &VeDescriptorSetLayout::Builder::addBinding(
	uint32_t binding,
	vk::DescriptorType descriptor_type,
	vk::ShaderStageFlags stage_flags,
	uint32_t count) {
	assert(m_bindings.count(binding) == 0 && "Binding already in use");
	vk::DescriptorSetLayoutBinding layout_binding{
		.binding = binding,
		.descriptorType = descriptor_type,
		.descriptorCount = count,
		.stageFlags = stage_flags,
		.pImmutableSamplers = nullptr
	};
	m_bindings[binding] = layout_binding;
	return *this;
}

VeDescriptorSetLayout::Builder &VeDescriptorSetLayout::Builder::setBindingFlags(
	uint32_t binding, vk::DescriptorBindingFlags flags) {
	m_binding_flags[binding] = flags;
	return *this;
}

VeDescriptorSetLayout::Builder &VeDescriptorSetLayout::Builder::setLayoutFlags(
	vk::DescriptorSetLayoutCreateFlags flags) {
	m_layout_flags = flags;
	return *this;
}

std::unique_ptr<VeDescriptorSetLayout> VeDescriptorSetLayout::Builder::build() const {
	return std::make_unique<VeDescriptorSetLayout>(m_ve_device, m_bindings, m_binding_flags, m_layout_flags);
}

// *************** Descriptor Set Layout *********************

VeDescriptorSetLayout::VeDescriptorSetLayout(
	VeDevice &device,
	std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> bindings_map,
	std::unordered_map<uint32_t, vk::DescriptorBindingFlags> binding_flags_map,
	vk::DescriptorSetLayoutCreateFlags layout_flags)
	: m_ve_device{device}, m_bindings{bindings_map} {
	std::vector<vk::DescriptorSetLayoutBinding> set_layout_bindings{};
	for (const auto& kv : bindings_map)
		set_layout_bindings.push_back(kv.second);

	// Sort bindings by binding number so flags array matches order
	std::sort(set_layout_bindings.begin(), set_layout_bindings.end(),
		[](const auto& a, const auto& b) { return a.binding < b.binding; });

	// Build per-binding flags array (in sorted order)
	std::vector<vk::DescriptorBindingFlags> binding_flags(set_layout_bindings.size(), vk::DescriptorBindingFlags{});
	bool has_flags = !binding_flags_map.empty();
	if (has_flags) {
		for (size_t i = 0; i < set_layout_bindings.size(); i++) {
			auto it = binding_flags_map.find(set_layout_bindings[i].binding);
			if (it != binding_flags_map.end())
				binding_flags[i] = it->second;
		}
	}

	vk::DescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
		.bindingCount = static_cast<uint32_t>(binding_flags.size()),
		.pBindingFlags = binding_flags.data()
	};

	vk::DescriptorSetLayoutCreateInfo descriptor_set_layout_info{
		.pNext = has_flags ? &binding_flags_info : nullptr,
		.flags = layout_flags,
		.bindingCount = static_cast<uint32_t>(set_layout_bindings.size()),
		.pBindings = set_layout_bindings.data()
	};

	m_descriptor_set_layout = vk::raii::DescriptorSetLayout(m_ve_device.getDevice(), descriptor_set_layout_info);
}

VeDescriptorSetLayout::~VeDescriptorSetLayout() {}

// *************** Descriptor Pool Builder *********************

VeDescriptorPool::Builder &VeDescriptorPool::Builder::addPoolSize(
		vk::DescriptorType descriptor_type, uint32_t count) {
	m_pool_sizes.push_back({descriptor_type, count});
	return *this;
}

VeDescriptorPool::Builder &VeDescriptorPool::Builder::setPoolFlags(
		vk::DescriptorPoolCreateFlags flags) {
	m_pool_flags = flags;
	return *this;
}
VeDescriptorPool::Builder &VeDescriptorPool::Builder::setMaxSets(uint32_t count) {
	m_max_sets = count;
	return *this;
}

std::unique_ptr<VeDescriptorPool> VeDescriptorPool::Builder::build() const {
	return std::make_unique<VeDescriptorPool>(m_ve_device, m_max_sets, m_pool_flags, m_pool_sizes);
}

std::shared_ptr<VeDescriptorPool> VeDescriptorPool::Builder::buildShared() const {
	return std::make_shared<VeDescriptorPool>(m_ve_device, m_max_sets, m_pool_flags, m_pool_sizes);
}

// *************** Descriptor Pool *********************

VeDescriptorPool::VeDescriptorPool(
		VeDevice &ve_device,
		uint32_t max_sets,
		vk::DescriptorPoolCreateFlags pool_flags,
		const std::vector<vk::DescriptorPoolSize> &pool_sizes)
		: m_ve_device{ve_device}, m_max_sets{max_sets}, m_pool_flags{pool_flags}, m_pool_sizes{pool_sizes} {
	addPool();
}

VeDescriptorPool::~VeDescriptorPool() {}

void VeDescriptorPool::addPool() {
	vk::DescriptorPoolCreateInfo info{
		.flags = m_pool_flags,
		.maxSets = m_max_sets,
		.poolSizeCount = static_cast<uint32_t>(m_pool_sizes.size()),
		.pPoolSizes = m_pool_sizes.data()
	};
	m_pools.emplace_back(m_ve_device.getDevice(), info);
	VE_LOGI("VeDescriptorPool: created pool page " << m_pools.size()
		<< " (maxSets=" << m_max_sets << ")");
}

void VeDescriptorPool::allocateFromPool(
	vk::raii::DescriptorPool& pool,
	const vk::raii::DescriptorSetLayout& layout,
	vk::raii::DescriptorSet& set,
	const void* p_next) {

	vk::DescriptorSetAllocateInfo alloc_info{
		.pNext = p_next,
		.descriptorPool = *pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &*layout
	};
	auto sets = vk::raii::DescriptorSets(m_ve_device.getDevice(), alloc_info);
	set = std::move(sets.front());
}

void VeDescriptorPool::allocateWithGrow(
	const vk::raii::DescriptorSetLayout& layout,
	vk::raii::DescriptorSet& set,
	const void* p_next) {

	try {
		allocateFromPool(m_pools.back(), layout, set, p_next);
	} catch (const vk::SystemError& e) {
		if (e.code() == vk::Result::eErrorOutOfPoolMemory || e.code() == vk::Result::eErrorFragmentedPool) {
			VE_LOGW("VeDescriptorPool: pool page " << m_pools.size()
				<< " exhausted, growing");
			addPool();
			allocateFromPool(m_pools.back(), layout, set, p_next);
		} else
			throw;
	}
}

void VeDescriptorPool::allocateDescriptor(
	const vk::raii::DescriptorSetLayout& descriptor_set_layout,
	vk::raii::DescriptorSet& descriptor_set) {
	allocateWithGrow(descriptor_set_layout, descriptor_set);
}

void VeDescriptorPool::allocateDescriptorVariableCount(
	const vk::raii::DescriptorSetLayout& descriptor_set_layout,
	vk::raii::DescriptorSet& descriptor_set, uint32_t variable_count) {

	vk::DescriptorSetVariableDescriptorCountAllocateInfo variable_info{
		.descriptorSetCount = 1,
		.pDescriptorCounts = &variable_count
	};
	allocateWithGrow(descriptor_set_layout, descriptor_set, &variable_info);
}

void VeDescriptorPool::resetPool() {
	vk::Device device = *m_ve_device.getDevice();
	for (auto& pool : m_pools)
		device.resetDescriptorPool(*pool);

	if (m_pools.size() > 1) {
		VE_LOGD("VeDescriptorPool: reset, shrinking from " << m_pools.size()
			<< " pages to 1");
		auto first = std::move(m_pools.front());
		m_pools.clear();
		m_pools.push_back(std::move(first));
	}
}

// *************** Descriptor Writer *********************

VeDescriptorWriter::VeDescriptorWriter(VeDescriptorSetLayout &set_layout, VeDescriptorPool &pool)
: m_set_layout{set_layout}, m_pool{pool} {}

VeDescriptorWriter &VeDescriptorWriter::writeBuffer(
	uint32_t binding, vk::DescriptorBufferInfo *buffer_info) {
	assert(m_set_layout.m_bindings.count(binding) == 1 && "Layout does not contain specified binding");

	auto &binding_description = m_set_layout.m_bindings[binding];

	assert(
		binding_description.descriptorCount == 1 &&
		"Binding single descriptor info, but binding expects multiple");

	vk::WriteDescriptorSet write{
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = binding_description.descriptorType,
		.pBufferInfo = buffer_info
	};

	m_writes.push_back(write);
	return *this;
}

VeDescriptorWriter &VeDescriptorWriter::writeImage(
	uint32_t binding, vk::DescriptorImageInfo *image_info) {
	assert(m_set_layout.m_bindings.count(binding) == 1 && "Layout does not contain specified binding");

	auto &binding_description = m_set_layout.m_bindings[binding];

	assert(
		binding_description.descriptorCount == 1 &&
		"Binding single descriptor info, but binding expects multiple");

	vk::WriteDescriptorSet write{
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = binding_description.descriptorType,
		.pImageInfo = image_info
	};
	m_writes.push_back(write);
	return *this;
}

VeDescriptorWriter &VeDescriptorWriter::writeImageArray(
	uint32_t binding, vk::DescriptorImageInfo *image_infos, uint32_t count) {
	assert(m_set_layout.m_bindings.count(binding) == 1 && "Layout does not contain specified binding");

	auto &binding_description = m_set_layout.m_bindings[binding];

	vk::WriteDescriptorSet write{
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = count,
		.descriptorType = binding_description.descriptorType,
		.pImageInfo = image_infos
	};
	m_writes.push_back(write);
	return *this;
}

void VeDescriptorWriter::build(vk::raii::DescriptorSet &set) {
	m_pool.allocateDescriptor(m_set_layout.getDescriptorSetLayout(), set);
	overwrite(set);
}

void VeDescriptorWriter::overwrite(vk::raii::DescriptorSet &set) {
	for (auto &write : m_writes) {
		write.dstSet = *set;
	}
	m_pool.m_ve_device.getDevice().updateDescriptorSets(m_writes, {});
}

}  // namespace ve