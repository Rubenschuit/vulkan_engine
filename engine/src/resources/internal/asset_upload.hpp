/* GPU upload helpers for asset loading.
 * Takes a LoadedAssetData produced by the glTF loader and produces resource
 * handles via VeResourceManager. Main-thread only (Vulkan calls).
 */
#pragma once
#include "ve_export.hpp"
#include "resources/internal/loaded_asset_data.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/internal/upload_context.hpp"

#include <cstdint>
#include <vector>

namespace ve {

struct UploadedHandles {
	std::vector<ResourceHandle<VeTexture>> textures;
	std::vector<ResourceHandle<VeMesh>> meshes;
	std::vector<ResourceHandle<VeMaterial>> materials;
};

// Cursor for the batched upload variant. Caller owns the lifetime.
struct UploadCursor {
	uint32_t tex = 0;
	uint32_t mesh = 0;
	uint32_t mat = 0;
};

// Batched: advances the cursor by up to `max_items` work units across textures,
// meshes, and materials. Texture and mesh uploads are interleaved so meshes.
// Records all GPU work into ctx's CBs. Stops early if ctx.bytes_in_flight
// passes max_staging_bytes between items. Returns true when fully drained.
bool uploadLoadedAssetStep(VeResourceManager& rm, LoadedAssetData& data,
                           UploadCursor& cursor, UploadedHandles& out,
                           uint32_t max_items, size_t max_staging_bytes,
                           UploadContext& ctx);

}