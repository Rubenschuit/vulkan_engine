#include "ve_descriptors.hpp"

// std
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
		.stageFlags = stage_flags
	};
	m_bindings[binding] = layout_binding;
	return *this;
}

std::unique_ptr<VeDescriptorSetLayout> VeDescriptorSetLayout::Builder::build() const {
	return std::make_unique<VeDescriptorSetLayout>(m_ve_device, m_bindings);
}

// *************** Descriptor Set Layout *********************

VeDescriptorSetLayout::VeDescriptorSetLayout(
	VeDevice &device, std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> bindings_map)
	: m_ve_device{device}, m_bindings{bindings_map} {
	std::vector<vk::DescriptorSetLayoutBinding> set_layout_bindings{};
	for (const auto& kv : bindings_map) {
		set_layout_bindings.push_back(kv.second);
	}

	vk::DescriptorSetLayoutCreateInfo descriptor_set_layout_info{
		.flags = {}, // TODO
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
		vk::DescriptorPoolCreateFlagBits flags) {
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
		vk::DescriptorPoolCreateFlagBits pool_flags,
		const std::vector<vk::DescriptorPoolSize> &pool_sizes)
		: m_ve_device{ve_device} {

	vk::DescriptorPoolCreateInfo descriptor_pool_info{
		.flags = pool_flags,
		.maxSets = max_sets,
		.poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
		.pPoolSizes = pool_sizes.data()
	};

	m_descriptor_pool = vk::raii::DescriptorPool(m_ve_device.getDevice(), descriptor_pool_info);
}

VeDescriptorPool::~VeDescriptorPool() {}

//TODO: consider allocating more than one at once and handling full pool
void VeDescriptorPool::allocateDescriptor(const vk::raii::DescriptorSetLayout& descriptor_set_layout, vk::raii::DescriptorSet& descriptor_set) const {

	vk::DescriptorSetAllocateInfo alloc_info{
		.descriptorPool = *m_descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &*descriptor_set_layout
	};

	auto descriptor_sets = vk::raii::DescriptorSets(m_ve_device.getDevice(), alloc_info);
	descriptor_set = std::move(descriptor_sets.front());
}

void VeDescriptorPool::resetPool() {
	vk::Device device = *m_ve_device.getDevice();
	device.resetDescriptorPool(m_descriptor_pool);
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