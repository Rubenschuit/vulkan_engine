/* VeMaterial - PBR material resource
 * Inherits from Resource for use with VeResourceManager.
 * Holds 7 texture handles; material data reaches shaders via 
 * MaterialSSBOManager (bindless lookup by index).
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_material_properties.hpp"

namespace ve {

// Texture handles for a PBR material. Empty slots are substituted with engine
// defaults by VeResourceManager::createMaterial.
struct MaterialTextures {
	ResourceHandle<VeTexture> albedo;
	ResourceHandle<VeTexture> normal;
	ResourceHandle<VeTexture> metallic_roughness;
	ResourceHandle<VeTexture> occlusion;
	ResourceHandle<VeTexture> emissive;
	ResourceHandle<VeTexture> specular;
	ResourceHandle<VeTexture> specular_color;
};

class VENGINE_API VeMaterial : public Resource {
public:
	VeMaterial(const std::string& resource_id,
	           MaterialTextures textures,
	           MaterialAlphaProps alpha_props,
	           MaterialFactors factors,
	           bool flip_tex_coord_v = false,
	           MaterialUvTransforms uv_transforms = {});
	~VeMaterial() override;

	VeMaterial(const VeMaterial&) = delete;
	VeMaterial& operator=(const VeMaterial&) = delete;

	MaterialAlphaProps getAlphaProps() const { return m_alpha_props; }
	void setAlphaProps(const MaterialAlphaProps& props) { m_alpha_props = props; }
	MaterialFactors getMaterialFactors() const { return m_factors; }
	void setMaterialFactors(const MaterialFactors& factors);
	// When true, vertex shader flips tex coord v. Required for some gltf exporters.
	bool getFlipTexCoordV() const { return m_flip_tex_coord_v; }
	const MaterialUvTransforms& getUvTransforms() const { return m_uv_transforms; }

	const ResourceHandle<VeTexture>& getAlbedoTexture() const { return m_textures.albedo; }
	const ResourceHandle<VeTexture>& getNormalTexture() const { return m_textures.normal; }
	const ResourceHandle<VeTexture>& getMetallicRoughnessTexture() const { return m_textures.metallic_roughness; }
	const ResourceHandle<VeTexture>& getOcclusionTexture() const { return m_textures.occlusion; }
	const ResourceHandle<VeTexture>& getEmissiveTexture() const { return m_textures.emissive; }
	const ResourceHandle<VeTexture>& getSpecularTexture() const { return m_textures.specular; }
	const ResourceHandle<VeTexture>& getSpecularColorTexture() const { return m_textures.specular_color; }

protected:
	bool doLoad() override { return true; }
	void doUnload() override {}
	void emitUnloadingEvent(EventBus& bus) override;

private:
	MaterialTextures m_textures;
	MaterialAlphaProps m_alpha_props;
	MaterialFactors m_factors;
	bool m_flip_tex_coord_v{false};
	MaterialUvTransforms m_uv_transforms;
};

} // namespace ve