/* Contains classes and builders for creating descriptor sets layouts,
descriptor pools, and writing descriptor sets.
*/
#pragma once

#include "ve_device.hpp"
#include "ve_export.hpp"

// std
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

class VENGINE_API VeDescriptorSetLayout {
public:

	class VENGINE_API Builder {
	public:
		Builder(VeDevice &ve_device) : m_ve_device{ve_device} {}

		Builder &addBinding(
			uint32_t binding,
			vk::DescriptorType descriptor_type,
			vk::ShaderStageFlags stage_flags,
			uint32_t count = 1);
		std::unique_ptr<VeDescriptorSetLayout> build() const;

	private:
		VeDevice &m_ve_device;
		std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> m_bindings{};
	};

	VeDescriptorSetLayout(VeDevice &ve_device, std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> bindings_map);
	~VeDescriptorSetLayout();

	VeDescriptorSetLayout(const VeDescriptorSetLayout &) = delete;
	VeDescriptorSetLayout &operator=(const VeDescriptorSetLayout &) = delete;

	const vk::raii::DescriptorSetLayout& getDescriptorSetLayout() const { return m_descriptor_set_layout; }

private:
	VeDevice &m_ve_device;
	vk::raii::DescriptorSetLayout m_descriptor_set_layout = nullptr;
	std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> m_bindings;

	friend class VeDescriptorWriter;
};

class VENGINE_API VeDescriptorPool {
public:

	class VENGINE_API Builder {
	public:
		Builder(VeDevice &ve_device) : m_ve_device{ve_device} {}

		Builder &addPoolSize(vk::DescriptorType descriptor_type, uint32_t count);
		Builder &setPoolFlags(vk::DescriptorPoolCreateFlagBits flags);
		Builder &setMaxSets(uint32_t count);
			std::unique_ptr<VeDescriptorPool> build() const;
			std::shared_ptr<VeDescriptorPool> buildShared() const;

	private:
		VeDevice &m_ve_device;
		std::vector<vk::DescriptorPoolSize> m_pool_sizes{};
		uint32_t m_max_sets = 1000;
		vk::DescriptorPoolCreateFlagBits m_pool_flags{};
	};

	VeDescriptorPool(
		VeDevice &ve_device,
		uint32_t max_sets,
		vk::DescriptorPoolCreateFlagBits pool_flags,
		const std::vector<vk::DescriptorPoolSize> &pool_sizes);
	~VeDescriptorPool();
	VeDescriptorPool(const VeDescriptorPool &) = delete;
	VeDescriptorPool &operator=(const VeDescriptorPool &) = delete;

	void allocateDescriptor(const vk::raii::DescriptorSetLayout& descriptor_set_layout, vk::raii::DescriptorSet& descriptor_set) const;

	void resetPool();

private:
	VeDevice &m_ve_device;
	vk::raii::DescriptorPool m_descriptor_pool = nullptr;
	friend class VeDescriptorWriter;
};

class VENGINE_API VeDescriptorWriter {
public:
	VeDescriptorWriter(VeDescriptorSetLayout &set_layout, VeDescriptorPool &pool);

	VeDescriptorWriter &writeBuffer(uint32_t binding, vk::DescriptorBufferInfo *buffer_info);
	VeDescriptorWriter &writeImage(uint32_t binding, vk::DescriptorImageInfo *image_info);

	void build(vk::raii::DescriptorSet &set);
	void overwrite(vk::raii::DescriptorSet &set);

private:
	VeDescriptorSetLayout &m_set_layout;
	VeDescriptorPool &m_pool;
	std::vector<vk::WriteDescriptorSet> m_writes;
};

} // namespace ve