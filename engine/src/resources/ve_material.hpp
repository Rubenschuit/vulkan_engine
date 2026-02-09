/* VeMaterial - PBR material resource (textures + descriptor set).
 * Inherits from Resource for use with VeResourceManager.
 * Holds albedo, normal, metallic-roughness textures and optional descriptor set.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_material_properties.hpp"

#include <filesystem>
#include <optional>

namespace ve {

class VeDescriptorPool;
class VeDescriptorSetLayout;

class VENGINE_API VeMaterial : public Resource {
public:
	// Create material from texture paths. pool and layout can be null for untextured materials.
	VeMaterial(VeResourceManager& resource_manager, const std::string& resource_id,
	           const std::filesystem::path& albedo_path,
	           const std::filesystem::path& normal_path,
	           const std::filesystem::path& metallic_roughness_path,
	           MaterialAlphaProps alpha_props,
	           VeDescriptorPool* pool, VeDescriptorSetLayout* layout);
	~VeMaterial() override;

	VeMaterial(const VeMaterial&) = delete;
	VeMaterial& operator=(const VeMaterial&) = delete;

	// Get descriptor set for PBR rendering. Valid only when created with pool/layout.
	vk::raii::DescriptorSet& getDescriptorSet();
	bool hasDescriptorSet() const { return m_descriptor_set.has_value(); }

	MaterialAlphaProps getAlphaProps() const { return m_alpha_props; }

protected:
	bool doLoad() override;
	void doUnload() override;

private:
	VeResourceManager* m_resource_manager;
	std::filesystem::path m_albedo_path;
	std::filesystem::path m_normal_path;
	std::filesystem::path m_metallic_roughness_path;
	MaterialAlphaProps m_alpha_props;
	VeDescriptorPool* m_pool;
	VeDescriptorSetLayout* m_layout;

	ResourceHandle<VeTexture> m_albedo_texture;
	ResourceHandle<VeTexture> m_normal_texture;
	ResourceHandle<VeTexture> m_metallic_roughness_texture;
	std::optional<vk::raii::DescriptorSet> m_descriptor_set;
};

} // namespace ve
