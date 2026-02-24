/* VeMaterial - PBR material resource (textures + descriptor set).
 * Inherits from Resource for use with VeResourceManager.
 * Holds albedo, normal, metallic-roughness, occlusion, emissive textures.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_material_properties.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace ve {

class VeBuffer;
class VeDescriptorPool;
class VeDescriptorSetLayout;

class VENGINE_API VeMaterial : public Resource {
public:
	// Create material from texture paths and PBR factors. pool and layout can be null for untextured materials.
	VeMaterial(VeResourceManager& resource_manager, const std::string& resource_id,
	           const std::filesystem::path& albedo_path,
	           const std::filesystem::path& normal_path,
	           const std::filesystem::path& metallic_roughness_path,
	           const std::filesystem::path& occlusion_path,
	           const std::filesystem::path& emissive_path,
	           MaterialAlphaProps alpha_props,
	           MaterialFactors factors,
	           VeDescriptorPool* pool, VeDescriptorSetLayout* layout,
	           bool flip_tex_coord_v = false);
	~VeMaterial() override;

	VeMaterial(const VeMaterial&) = delete;
	VeMaterial& operator=(const VeMaterial&) = delete;

	// Get descriptor set for PBR rendering. Valid only when created with pool/layout.
	vk::raii::DescriptorSet& getDescriptorSet();
	bool hasDescriptorSet() const { return m_descriptor_set.has_value(); }

	MaterialAlphaProps getAlphaProps() const { return m_alpha_props; }
	void setAlphaProps(const MaterialAlphaProps& props) { m_alpha_props = props; }
	MaterialFactors getMaterialFactors() const { return m_factors; }
	void setMaterialFactors(const MaterialFactors& factors);
	// When true, vertex shader flips tex coord v. Required for some gltf exporters.
	bool getFlipTexCoordV() const { return m_flip_tex_coord_v; }

	// Texture access for editor UI (read-only display)
	const ResourceHandle<VeTexture>& getAlbedoTexture() const { return m_albedo_texture; }
	const ResourceHandle<VeTexture>& getNormalTexture() const { return m_normal_texture; }
	const ResourceHandle<VeTexture>& getMetallicRoughnessTexture() const { return m_metallic_roughness_texture; }
	const ResourceHandle<VeTexture>& getOcclusionTexture() const { return m_occlusion_texture; }
	const ResourceHandle<VeTexture>& getEmissiveTexture() const { return m_emissive_texture; }

protected:
	bool doLoad() override;
	void doUnload() override;

private:
	VeResourceManager* m_resource_manager;
	std::filesystem::path m_albedo_path;
	std::filesystem::path m_normal_path;
	std::filesystem::path m_metallic_roughness_path;
	std::filesystem::path m_occlusion_path;
	std::filesystem::path m_emissive_path;
	MaterialAlphaProps m_alpha_props;
	MaterialFactors m_factors;
	VeDescriptorPool* m_pool;
	VeDescriptorSetLayout* m_layout;
	bool m_flip_tex_coord_v{false};

	ResourceHandle<VeTexture> m_albedo_texture;
	ResourceHandle<VeTexture> m_normal_texture;
	ResourceHandle<VeTexture> m_metallic_roughness_texture;
	ResourceHandle<VeTexture> m_occlusion_texture;
	ResourceHandle<VeTexture> m_emissive_texture;
	std::unique_ptr<VeBuffer> m_material_ubo;
	std::optional<vk::raii::DescriptorSet> m_descriptor_set;
};

} // namespace ve
