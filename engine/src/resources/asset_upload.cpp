#include "pch.hpp"
#include "resources/internal/asset_upload.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_texture.hpp"

namespace ve {

// Writes the resulting handle into out_textures[idx] (parallel to data.textures).
// Empty-pixel entries (decode failures) leave the slot empty; createMaterial
// substitutes the default for that slot by type.
static void uploadOneTexture(VeResourceManager& rm, DecodedTexture& tex, size_t idx,
                             std::vector<ResourceHandle<VeTexture>>& out_textures,
                             UploadContext& ctx) {
	if (tex.pixels.empty())
		return;
	std::string key = VeTexture::makeResourceKey(tex.file_path, tex.type);
	auto handle = rm.createTextureFromData(key, tex, ctx);
	if (handle)
		out_textures[idx] = std::move(handle);
	tex.pixels.clear();
	tex.pixels.shrink_to_fit();
	tex.mip_offsets.clear();
	tex.mip_offsets.shrink_to_fit();
	tex.mip_extents.clear();
	tex.mip_extents.shrink_to_fit();
}

static void uploadOneMesh(VeResourceManager& rm, ProcessedMesh& mesh,
                          std::vector<ResourceHandle<VeMesh>>& out_meshes,
                          UploadContext& ctx) {
	auto handle = rm.createMeshFromData(mesh.resource_id, mesh, ctx);
	out_meshes.push_back(std::move(handle));
	mesh.vertices.clear();
	mesh.vertices.shrink_to_fit();
	mesh.indices.clear();
	mesh.indices.shrink_to_fit();
	mesh.lod_indices.clear();
	mesh.lod_indices.shrink_to_fit();
}

// Out-of-range or empty slots return an empty handle
static ResourceHandle<VeTexture> resolveSlot(const std::vector<ResourceHandle<VeTexture>>& textures, int idx) {
	if (idx < 0 || static_cast<size_t>(idx) >= textures.size())
		return {};
	return textures[static_cast<size_t>(idx)];
}

static ResourceHandle<VeMaterial> createOneMaterial(VeResourceManager& rm,
                                                    const std::vector<ResourceHandle<VeTexture>>& uploaded_textures,
                                                    const ProcessedMaterial& pm) {
	MaterialTextures textures{
		.albedo             = resolveSlot(uploaded_textures, pm.albedo_tex_idx),
		.normal             = resolveSlot(uploaded_textures, pm.normal_tex_idx),
		.metallic_roughness = resolveSlot(uploaded_textures, pm.metallic_roughness_tex_idx),
		.occlusion          = resolveSlot(uploaded_textures, pm.occlusion_tex_idx),
		.emissive           = resolveSlot(uploaded_textures, pm.emissive_tex_idx),
		.specular           = resolveSlot(uploaded_textures, pm.specular_tex_idx),
		.specular_color     = resolveSlot(uploaded_textures, pm.specular_color_tex_idx),
	};
	return rm.createMaterial(pm.resource_id, std::move(textures),
	                         pm.alpha_props, pm.factors, pm.flip_tex_coord_v, pm.uv_transforms);
}

// Highest non-negative texture slot index referenced by this material, or -1 if none.
static int maxReferencedTexIdx(const ProcessedMaterial& pm) {
	return std::max({pm.albedo_tex_idx, pm.normal_tex_idx, pm.metallic_roughness_tex_idx,
	                 pm.occlusion_tex_idx, pm.emissive_tex_idx,
	                 pm.specular_tex_idx, pm.specular_color_tex_idx});
}

bool uploadLoadedAssetStep(VeResourceManager& rm, LoadedAssetData& data,
                           UploadCursor& cursor, UploadedHandles& out,
                           uint32_t max_items, size_t max_staging_bytes,
                           UploadContext& ctx) {
	// out.textures is parallel to data.textures so material slots can index it directly
	if (out.textures.size() < data.textures.size())
		out.textures.resize(data.textures.size());

	uint32_t budget = 0;

	// A material is eligible once every texture slot it references has been
	// attempted.
	auto drainEligibleMaterials = [&]() {
		while (cursor.mat < data.materials.size() && budget < max_items) {
			int max_dep = maxReferencedTexIdx(data.materials[cursor.mat]);
			if (max_dep >= 0 && static_cast<uint32_t>(max_dep) >= cursor.tex)
				break;
			out.materials.push_back(createOneMaterial(rm, out.textures, data.materials[cursor.mat]));
			cursor.mat++;
			budget++;
		}
	};

	// Interleave texture and mesh uploads
	bool progressed = true;
	while (progressed && budget < max_items) {
		progressed = false;

		if (cursor.tex < data.textures.size()
		    && budget < max_items
		    && ctx.bytes_in_flight < max_staging_bytes) {
			uploadOneTexture(rm, data.textures[cursor.tex], cursor.tex, out.textures, ctx);
			cursor.tex++;
			budget++;
			progressed = true;
		}

		drainEligibleMaterials();

		if (cursor.mesh < data.meshes.size()
		    && budget < max_items
		    && ctx.bytes_in_flight < max_staging_bytes) {
			uploadOneMesh(rm, data.meshes[cursor.mesh], out.meshes, ctx);
			cursor.mesh++;
			budget++;
			progressed = true;
		}
	}

	return cursor.tex >= data.textures.size()
	    && cursor.mesh >= data.meshes.size()
	    && cursor.mat >= data.materials.size();
}

} // namespace ve