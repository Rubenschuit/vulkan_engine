#include "ve_descriptors.hpp"

// std
#include <cassert>
#include <stdexcept>

namespace ve {

// *************** Descriptor Set Layout Builder *********************

VeDescriptorSetLayout::Builder &VeDescriptorSetLayout::Builder::addBinding(
		uint32_t binding,
		vk::DescriptorType descriptor_type,
		vk::ShaderStageFlagBits stage_flags,
		uint32_t count) {
	assert(bindings.count(binding) == 0 && "Binding already in use");
	vk::DescriptorSetLayoutBinding layout_binding{
		.binding = binding,
		.descriptorType = descriptor_type,
		.stageFlags = stage_flags,
		.descriptorCount = count
	};
	bindings[binding] = layout_binding;
	return *this;
}

std::unique_ptr<VeDescriptorSetLayout> VeDescriptorSetLayout::Builder::build() const {
	return std::make_unique<VeDescriptorSetLayout>(ve_device, bindings);
}

// *************** Descriptor Set Layout *********************

VeDescriptorSetLayout::VeDescriptorSetLayout(
	VeDevice &device, std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> bindings_map)
	: ve_device{device}, bindings{bindings_map} {
	std::vector<vk::DescriptorSetLayoutBinding> set_layout_bindings{};
	for (auto kv : bindings_map) {
		set_layout_bindings.push_back(kv.second);
	}

	vk::DescriptorSetLayoutCreateInfo descriptor_set_layout_info{
		.flags = {}, // TODO
		.bindingCount = static_cast<uint32_t>(set_layout_bindings.size()),
		.pBindings = set_layout_bindings.data()
	};

	descriptor_set_layout = vk::raii::DescriptorSetLayout(ve_device.getDevice(), descriptor_set_layout_info);
}

VeDescriptorSetLayout::~VeDescriptorSetLayout() {}

// *************** Descriptor Pool Builder *********************

VeDescriptorPool::Builder &VeDescriptorPool::Builder::addPoolSize(
		vk::DescriptorType descriptor_type, uint32_t count) {
	pool_sizes.push_back({descriptor_type, count});
	return *this;
}

VeDescriptorPool::Builder &VeDescriptorPool::Builder::setPoolFlags(
		vk::DescriptorPoolCreateFlagBits flags) {
	pool_flags = flags;
	return *this;
}
VeDescriptorPool::Builder &VeDescriptorPool::Builder::setMaxSets(uint32_t count) {
	max_sets = count;
	return *this;
}

std::unique_ptr<VeDescriptorPool> VeDescriptorPool::Builder::build() const {
	return std::make_unique<VeDescriptorPool>(ve_device, max_sets, pool_flags, pool_sizes);
}

// *************** Descriptor Pool *********************

VeDescriptorPool::VeDescriptorPool(
		VeDevice &ve_device,
		uint32_t max_sets,
		vk::DescriptorPoolCreateFlagBits pool_flags,
		const std::vector<vk::DescriptorPoolSize> &pool_sizes)
		: ve_device{ve_device} {

	vk::DescriptorPoolCreateInfo descriptor_pool_info{
		.flags = pool_flags,
		.maxSets = max_sets,
		.poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
		.pPoolSizes = pool_sizes.data()
	};

	descriptor_pool = vk::raii::DescriptorPool(ve_device.getDevice(), descriptor_pool_info);
}

VeDescriptorPool::~VeDescriptorPool() {}

//TODO: consider allocating more than one at once and handling full pool
void VeDescriptorPool::allocateDescriptor(const vk::raii::DescriptorSetLayout& descriptor_set_layout, vk::raii::DescriptorSet& descriptor_set) const {

	vk::DescriptorSetAllocateInfo alloc_info{
		.descriptorPool = *descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &*descriptor_set_layout
	};

	auto descriptor_sets = vk::raii::DescriptorSets(ve_device.getDevice(), alloc_info);
	descriptor_set = std::move(descriptor_sets.front());
}

void VeDescriptorPool::resetPool() {
	vk::Device device = *ve_device.getDevice();
	device.resetDescriptorPool(descriptor_pool);
}

// *************** Descriptor Writer *********************

VeDescriptorWriter::VeDescriptorWriter(VeDescriptorSetLayout &set_layout, VeDescriptorPool &pool)
  : set_layout{set_layout}, pool{pool} {}

VeDescriptorWriter &VeDescriptorWriter::writeBuffer(
	uint32_t binding, vk::DescriptorBufferInfo *buffer_info) {
	assert(set_layout.bindings.count(binding) == 1 && "Layout does not contain specified binding");

	auto &binding_description = set_layout.bindings[binding];

	assert(
		binding_description.descriptorCount == 1 &&
		"Binding single descriptor info, but binding expects multiple");

	vk::WriteDescriptorSet write{
		.descriptorType = binding_description.descriptorType,
		.dstBinding = binding,
		.pBufferInfo = buffer_info,
		.descriptorCount = 1
	};

	writes.push_back(write);
	return *this;
}

VeDescriptorWriter &VeDescriptorWriter::writeImage(
	uint32_t binding, vk::DescriptorImageInfo *image_info) {
	assert(set_layout.bindings.count(binding) == 1 && "Layout does not contain specified binding");

	auto &binding_description = set_layout.bindings[binding];

	assert(
		binding_description.descriptorCount == 1 &&
		"Binding single descriptor info, but binding expects multiple");

	vk::WriteDescriptorSet write{
		.descriptorType = binding_description.descriptorType,
		.dstBinding = binding,
		.pImageInfo = image_info,
		.descriptorCount = 1
	};
	writes.push_back(write);
	return *this;
}

void VeDescriptorWriter::build(vk::raii::DescriptorSet &set) {
	pool.allocateDescriptor(set_layout.getDescriptorSetLayout(), set);
	overwrite(set);
}

void VeDescriptorWriter::overwrite(vk::raii::DescriptorSet &set) {
	for (auto &write : writes) {
		write.dstSet = *set;
	}
	pool.ve_device.getDevice().updateDescriptorSets(writes, {});
}

}  // namespace ve