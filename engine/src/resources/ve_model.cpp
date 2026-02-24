#include "pch.hpp"
#include "resources/ve_model.hpp"
#include "resources/ve_texture.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_log.hpp"

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <mikktspace.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <numeric>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace ve {

uint32_t VeModel::s_next_node_id = 0;

// Return image index for a glTF texture: texture.source or KHR_texture_basisu.source. Returns -1 if invalid.
static int getTextureImageIndex(const tinygltf::Model& gltf, size_t tex_idx) {
	if (tex_idx >= gltf.textures.size())
		return -1;
	const tinygltf::Texture& tex = gltf.textures[tex_idx];
	if (tex.source >= 0 && static_cast<size_t>(tex.source) < gltf.images.size())
		return tex.source;
	auto it = tex.extensions.find("KHR_texture_basisu");
	if (it != tex.extensions.end()) {
		const tinygltf::Value& ext = it->second;
		if (ext.Has("source") && ext.Get("source").IsInt()) {
			int src = ext.Get("source").Get<int>();
			if (src >= 0 && static_cast<size_t>(src) < gltf.images.size())
				return src;
		}
	}
	return -1;
}

// Check for KTX/KTX2 magic bytes: 0xAB 'K' 'T' 'X'
static bool isKtxMagic(const unsigned char* bytes, size_t size) {
	return size >= 4 && bytes[0] == 0xAB && bytes[1] == 0x4B && bytes[2] == 0x54 && bytes[3] == 0x58;
}

// Custom image loader: handles KTX/KTX2 bypass and embedded vs external images.
// - KTX + embedded (glb): keep raw bytes in image->image for later in-memory loading
// - KTX + external (gltf): clear image->image, engine loads from file via URI
// - Non-KTX + embedded: decode to RGBA pixels via tinygltf (as_is=false)
// - Non-KTX + external: store as-is, engine loads from file via URI
static bool LoadImageDataVeModel(tinygltf::Image* image, const int image_idx, std::string* err,
                                 std::string* warn, int req_width, int req_height,
                                 const unsigned char* bytes, int size, void* user_data) {
	bool is_ktx = isKtxMagic(bytes, static_cast<size_t>(size));
	if (is_ktx) {
		image->width = image->height = image->component = -1;
		image->bits = image->pixel_type = -1;
		image->as_is = true;
		if (image->uri.empty()) // embedded (glb): keep raw KTX bytes for in-memory loading
			image->image.assign(bytes, bytes + size);
		else // external (gltf): engine loads from file via URI
			image->image.clear();
		return true;
	}
	if (image->uri.empty()) {
		// Embedded image (glb): decode PNG/JPEG to RGBA pixels
		tinygltf::LoadImageDataOption opt;
		opt.as_is = false;
		opt.preserve_channels = false;
		return tinygltf::LoadImageData(image, image_idx, err, warn, req_width, req_height, bytes, size, &opt);
	}
	// External image (gltf): store as-is, engine loads from file via URI
	return tinygltf::LoadImageData(image, image_idx, err, warn, req_width, req_height, bytes, size, user_data);
}

// Resolve texture path from glTF image. External images use URI on disk; embedded images (glb)
// return a synthetic "@embedded:" key that maps to the in-memory texture cache.
static std::filesystem::path resolveTexturePath(const tinygltf::Model& gltf, size_t tex_idx,
                                                const std::filesystem::path& model_dir,
                                                const std::string& model_path_str,
                                                const std::filesystem::path& default_path) {
	int img_idx = getTextureImageIndex(gltf, tex_idx);
	if (img_idx < 0) return default_path;
	const auto& image = gltf.images[static_cast<size_t>(img_idx)];
	if (!image.uri.empty())
		return model_dir / image.uri;
	std::string emb_key = "@embedded:" + model_path_str + "::img_" + std::to_string(img_idx);
	if (VeTexture::hasEmbedded(emb_key))
		return std::filesystem::path(emb_key);
	return default_path;
}

// Try to derive a texture path by replacing a known suffix with alternatives; return first path that exists.
static std::filesystem::path tryDerivePath(const std::filesystem::path& base,
                                           const std::vector<std::string>& from_suffixes,
                                           const std::vector<std::string>& to_suffixes) {
	std::string base_str = base.generic_string();
	std::string base_lower = base_str;
	std::transform(base_lower.begin(), base_lower.end(), base_lower.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (const std::string& from : from_suffixes) {
		size_t pos = base_lower.rfind(from);
		if (pos == std::string::npos || pos + from.size() > base_str.size()) continue;
		for (const std::string& to : to_suffixes) {
			std::string candidate_str = base_str;
			candidate_str.replace(pos, from.size(), to);
			std::filesystem::path candidate(candidate_str);
			if (std::filesystem::exists(candidate))
				return candidate;
		}
	}
	return {};
}

// MikkTSpace bridge: C callbacks use this to read/write VeMesh::Vertex data (engine Z-up space).
struct MikkTSpaceBridge {
	std::vector<VeMesh::Vertex>* vertices;
	const std::vector<uint32_t>* indices;
};
static int mikkGetNumFaces(const SMikkTSpaceContext* pContext) {
	auto* b = static_cast<const MikkTSpaceBridge*>(pContext->m_pUserData);
	return static_cast<int>(b->indices->size() / 3);
}
static int mikkGetNumVerticesOfFace(const SMikkTSpaceContext* /*pContext*/, const int /*iFace*/) {
	return 3;
}
static void mikkGetPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert) {
	auto* b = static_cast<const MikkTSpaceBridge*>(pContext->m_pUserData);
	uint32_t idx = (*b->indices)[static_cast<size_t>(iFace * 3 + iVert)];
	const glm::vec3& p = (*b->vertices)[idx].pos;
	fvPosOut[0] = p.x;
	fvPosOut[1] = p.y;
	fvPosOut[2] = p.z;
}
static void mikkGetNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert) {
	auto* b = static_cast<const MikkTSpaceBridge*>(pContext->m_pUserData);
	uint32_t idx = (*b->indices)[static_cast<size_t>(iFace * 3 + iVert)];
	const glm::vec3& n = (*b->vertices)[idx].normal;
	fvNormOut[0] = n.x;
	fvNormOut[1] = n.y;
	fvNormOut[2] = n.z;
}
static void mikkGetTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert) {
	auto* b = static_cast<const MikkTSpaceBridge*>(pContext->m_pUserData);
	uint32_t idx = (*b->indices)[static_cast<size_t>(iFace * 3 + iVert)];
	const glm::vec2& uv = (*b->vertices)[idx].tex_coord;
	fvTexcOut[0] = uv.x;
	fvTexcOut[1] = uv.y;
}
static void mikkSetTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert) {
	auto* b = static_cast<MikkTSpaceBridge*>(const_cast<void*>(pContext->m_pUserData));
	uint32_t idx = (*b->indices)[static_cast<size_t>(iFace * 3 + iVert)];
	VeMesh::Vertex& v = (*b->vertices)[idx];
	v.tangent.x = fvTangent[0];
	v.tangent.y = fvTangent[1];
	v.tangent.z = fvTangent[2];
	v.tangent.w = (fSign >= 0.0f) ? 1.0f : -1.0f;
}

// Y-up (glTF) to Z-up (engine): map (x,y,z) -> (x,-z,y)
static const glm::mat4 YUP_TO_ZUP(1.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f,
                                      0.0f, -1.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f);
static const glm::mat4 ZUP_TO_YUP(1.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, -1.0f, 0.0f,
                                      0.0f, 1.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f);

// Blender's glTF exporter bakes an internal radiance multiplier of ~638 into emissive values.
// Compensate so emissive intensities match other exporters. See glTF-Blender-IO #2473.
static constexpr float BLENDER_EMISSIVE_FACTOR = 638.0f;

// Emissive-derived point lights scaling
static constexpr float EMISSIVE_LIGHT_INTENSITY_SCALE = 10.0f;

// KHR_lights_punctual uses physical units (lux for directional, candela for point).
// Our engine uses arbitrary intensity values (~1-10 for directional, ~50-200 for point).
// This scale bridges the gap until we add EV100-based exposure.
static constexpr float KHR_PUNCTUAL_INTENSITY_SCALE = 1.0f / 1000.0f;

// Build local node matrix in glTF space (column-major: T*R*S).
static glm::mat4 getNodeMatrixGltf(const tinygltf::Node& node) {
	if (node.matrix.size() == 16) {
		return glm::mat4(
		    static_cast<float>(node.matrix[0]), static_cast<float>(node.matrix[1]), static_cast<float>(node.matrix[2]), static_cast<float>(node.matrix[3]),
		    static_cast<float>(node.matrix[4]), static_cast<float>(node.matrix[5]), static_cast<float>(node.matrix[6]), static_cast<float>(node.matrix[7]),
		    static_cast<float>(node.matrix[8]), static_cast<float>(node.matrix[9]), static_cast<float>(node.matrix[10]), static_cast<float>(node.matrix[11]),
		    static_cast<float>(node.matrix[12]), static_cast<float>(node.matrix[13]), static_cast<float>(node.matrix[14]), static_cast<float>(node.matrix[15]));
	}
	glm::vec3 t(0, 0, 0);
	if (node.translation.size() >= 3)
		t = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2])};
	glm::quat r(1, 0, 0, 0);
	if (node.rotation.size() >= 4)
		r = {static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])};
	glm::vec3 s(1, 1, 1);
	if (node.scale.size() >= 3)
		s = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2])};
	return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

// Convert node transform from glTF Y-up to engine Z-up: M_engine = C * M * C_inv, then decompose.
struct NodeTRS {
	glm::vec3 translation;
	glm::vec3 scale;
	glm::quat rotation;
};
static NodeTRS getNodeTRS(const tinygltf::Node& node) {
	const glm::mat4 M_gltf = getNodeMatrixGltf(node);
	const glm::mat4 M_engine = YUP_TO_ZUP * M_gltf * ZUP_TO_YUP;
	glm::vec3 scale;
	glm::quat rotation;
	glm::vec3 translation;
	glm::vec3 skew;
	glm::vec4 perspective;
	if (glm::decompose(M_engine, scale, rotation, translation, skew, perspective)) {
		return {translation, scale, rotation};
	}
	return {{0, 0, 0}, {1, 1, 1}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
}

// Position dedup: quantize positions to 1cm grid to avoid duplicate lights
struct PosKey {
	int32_t x, y, z;
	bool operator==(const PosKey&) const = default;
};
struct PosHash {
	size_t operator()(const PosKey& k) const {
		size_t h = std::hash<int32_t>{}(k.x);
		h ^= std::hash<int32_t>{}(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= std::hash<int32_t>{}(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};
using PosDedup = std::unordered_set<PosKey, PosHash>;
static PosKey quantize(const glm::vec3& p) {
	return {static_cast<int32_t>(std::round(p.x * 100.f)),
	        static_cast<int32_t>(std::round(p.y * 100.f)),
	        static_cast<int32_t>(std::round(p.z * 100.f))};
}

// Lightweight reference to a node+primitive for emissive extraction
struct NodePrim { int node_idx; std::string key; size_t mat_idx; };
using GeometryCenterExtent = std::unordered_map<std::string, std::pair<glm::vec3, float>>;

// Compute world matrices for all nodes in engine (Z-up) space
static std::vector<glm::mat4> computeNodeWorldMatrices(
    const tinygltf::Model& gltf, const std::vector<int>& root_nodes) {
	std::vector<glm::mat4> node_world_engine(gltf.nodes.size(), glm::mat4(1.0f));
	std::function<void(int, const glm::mat4&)> traverse = [&](int node_idx, const glm::mat4& parent_world) {
		if (node_idx < 0 || node_idx >= static_cast<int>(gltf.nodes.size())) return;
		const auto& node = gltf.nodes[static_cast<size_t>(node_idx)];
		glm::mat4 local_gltf = getNodeMatrixGltf(node);
		glm::mat4 local_engine = YUP_TO_ZUP * local_gltf * ZUP_TO_YUP;
		glm::mat4 world = parent_world * local_engine;
		node_world_engine[static_cast<size_t>(node_idx)] = world;
		for (int c : node.children)
			traverse(c, world);
	};
	for (int r : root_nodes)
		traverse(r, glm::mat4(1.0f));
	return node_world_engine;
}

// Cluster vertex positions along dominant axis. Returns centroids.
// For small or compact meshes, returns single centroid.
// For elongated meshes (e.g. string lights with multiple bulbs), uses adaptive
// gap detection to find natural breaks in the vertex distribution.
static std::vector<glm::vec3> clusterVertices(
    const std::vector<glm::vec3>& positions, float gap_threshold, float extent_threshold = 0.02f) {
	if (positions.empty())
		return {};

	// Find bounding box extent
	glm::vec3 mn = positions[0], mx = positions[0];
	for (const auto& p : positions) {
		mn = glm::min(mn, p);
		mx = glm::max(mx, p);
	}
	glm::vec3 ext = mx - mn;

	// Find dominant axis
	int dom = 0;
	if (ext.y > ext[dom])
		dom = 1;
	if (ext.z > ext[dom])
		dom = 2;

	if (ext[dom] < extent_threshold) {
		// Small mesh: single centroid
		return {(mn + mx) * 0.5f};
	}

	// Compact bounding box (roughly cubic/spherical): single light source.
	float min_ext = std::min({ext.x, ext.y, ext.z});
	if (min_ext > 1e-6f && ext[dom] / min_ext < 3.0f) {
		return {(mn + mx) * 0.5f};
	}

	// Sort vertex indices by dominant axis
	std::vector<size_t> sorted_idx(positions.size());
	std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
	std::sort(sorted_idx.begin(), sorted_idx.end(),
	          [&](size_t a, size_t b) { return positions[a][dom] < positions[b][dom]; });

	// Compute consecutive gaps along dominant axis
	std::vector<float> gaps(sorted_idx.size() - 1);
	for (size_t i = 0; i + 1 < sorted_idx.size(); i++)
		gaps[i] = positions[sorted_idx[i + 1]][dom] - positions[sorted_idx[i]][dom];

	// Adaptive gap detection: find natural breaks in the gap distribution.
	std::vector<float> sorted_gaps(gaps);
	std::sort(sorted_gaps.begin(), sorted_gaps.end());

	float best_ratio = 1.0f;
	size_t best_idx = 0;
	for (size_t i = 0; i + 1 < sorted_gaps.size(); i++) {
		if (sorted_gaps[i] < 1e-7f) continue;
		float ratio = sorted_gaps[i + 1] / sorted_gaps[i];
		if (ratio > best_ratio) {
			best_ratio = ratio;
			best_idx = i;
		}
	}

	constexpr float JUMP_RATIO_THRESHOLD = 5.0f;
	if (best_ratio <= JUMP_RATIO_THRESHOLD) {
		// No clear separation between gap sizes: single light source
		return {(mn + mx) * 0.5f};
	}

	// Bimodal distribution: use geometric mean at the boundary as threshold
	float effective_gap = std::max(gap_threshold,
	    std::sqrt(sorted_gaps[best_idx] * sorted_gaps[best_idx + 1]));

	// Cluster: split when gap between consecutive sorted vertices > threshold
	std::vector<glm::vec3> centroids;
	glm::vec3 sum = positions[sorted_idx[0]];
	size_t count = 1;
	for (size_t i = 1; i < sorted_idx.size(); i++) {
		if (gaps[i - 1] > effective_gap) {
			centroids.push_back(sum / static_cast<float>(count));
			sum = glm::vec3(0.f);
			count = 0;
		}
		sum += positions[sorted_idx[i]];
		count++;
	}
	centroids.push_back(sum / static_cast<float>(count));
	return centroids;
}

// Extract KHR_lights_punctual lights from glTF extension data
static std::vector<VeModel::ExtractedLight> extractPunctualLights(
    const tinygltf::Model& gltf,
    const std::vector<glm::mat4>& node_world_engine) {

	std::vector<VeModel::ExtractedLight> punctual_lights;
	auto it_pl = gltf.extensions.find("KHR_lights_punctual");
	if (it_pl == gltf.extensions.end() || !it_pl->second.Has("lights") || !it_pl->second.Get("lights").IsArray())
		return punctual_lights;

	const auto& arr = it_pl->second.Get("lights").Get<tinygltf::Value::Array>();
	for (size_t i = 0; i < arr.size(); i++) {
		const auto& v = arr[i];
		if (!v.IsObject())
			continue;
		VeModel::ExtractedLight light;
		if (v.Has("type") && v.Get("type").IsString()) {
			const std::string& type_str = v.Get("type").Get<std::string>();
			if (type_str == "directional")
				light.type = VeModel::ExtractedLightType::Directional;
			else
				light.type = VeModel::ExtractedLightType::Point;
		}
		if (v.Has("color") && v.Get("color").IsArray()) {
			const auto& c = v.Get("color").Get<tinygltf::Value::Array>();
			if (c.size() >= 3) {
				light.color.x = c[0].IsNumber() ? static_cast<float>(c[0].Get<double>()) : 1.f;
				light.color.y = c[1].IsNumber() ? static_cast<float>(c[1].Get<double>()) : 1.f;
				light.color.z = c[2].IsNumber() ? static_cast<float>(c[2].Get<double>()) : 1.f;
			}
		}
		if (v.Has("intensity") && v.Get("intensity").IsNumber())
			light.intensity = static_cast<float>(v.Get("intensity").Get<double>());
		if (v.Has("range") && v.Get("range").IsNumber())
			light.range = static_cast<float>(v.Get("range").Get<double>());
		if (v.Has("name") && v.Get("name").IsString())
			light.name = v.Get("name").Get<std::string>();
		punctual_lights.push_back(light);
	}
	for (size_t node_idx = 0; node_idx < gltf.nodes.size(); node_idx++) {
		const auto& node = gltf.nodes[node_idx];
		auto nit = node.extensions.find("KHR_lights_punctual");
		if (nit == node.extensions.end() || !nit->second.Has("light") || !nit->second.Get("light").IsInt())
			continue;
		int light_idx = nit->second.Get("light").Get<int>();
		if (light_idx < 0 || light_idx >= static_cast<int>(punctual_lights.size()))
			continue;
		const glm::mat4& W = node_world_engine[node_idx];
		auto& pl = punctual_lights[static_cast<size_t>(light_idx)];
		pl.position = glm::vec3(W * glm::vec4(0, 0, 0, 1));
		pl.direction = glm::normalize(glm::mat3(W) * glm::vec3(0, 0, -1));
		if (pl.name.empty() && !node.name.empty())
			pl.name = node.name;
		VE_LOGI("Extracted light " << pl.name << " from node " << node.name);
	}
	return punctual_lights;
}

// Extract lights from emissive materials, with vertex clustering for large meshes
static std::vector<VeModel::ExtractedLight> extractEmissiveLights(
    const tinygltf::Model& gltf,
    const std::vector<glm::mat4>& node_world_engine,
    const std::vector<NodePrim>& node_primitives,
    const GeometryCenterExtent& geometry_center_extent,
    const std::vector<MaterialFactors>& material_factors,
    PosDedup& dedup) {

	std::vector<VeModel::ExtractedLight> emissive_lights;
	uint32_t emissive_light_count = 0;
	const float emissive_light_threshold = 0.1f;
	constexpr float EMISSIVE_CLUSTER_GAP = 0.005f;
	constexpr float EMISSIVE_CLUSTER_EXTENT = 0.02f;

	auto findPositionAccessor = [&](const NodePrim& np) -> const tinygltf::Accessor* {
		int mesh_idx = gltf.nodes[static_cast<size_t>(np.node_idx)].mesh;
		if (mesh_idx < 0)
			return nullptr;
		const auto& mesh = gltf.meshes[static_cast<size_t>(mesh_idx)];
		for (const auto& prim : mesh.primitives) {
			size_t pmat = (prim.material >= 0) ? static_cast<size_t>(prim.material) : 0;
			if (pmat != np.mat_idx) continue;
			auto pos_it = prim.attributes.find("POSITION");
			if (pos_it == prim.attributes.end()) continue;
			return &gltf.accessors[static_cast<size_t>(pos_it->second)];
		}
		return nullptr;
	};

	for (size_t mat_i = 0; mat_i < material_factors.size(); mat_i++) {
		float chroma = glm::length(material_factors[mat_i].emissive_factor);
		float strength = material_factors[mat_i].emissive_strength;
		if (chroma * strength < emissive_light_threshold)
			continue;
		glm::vec3 color_n = (chroma > 1e-6f) ? (material_factors[mat_i].emissive_factor / chroma) : material_factors[mat_i].emissive_factor;
		for (const NodePrim& np : node_primitives) {
			if (np.mat_idx != mat_i)
				continue;
			auto ce_it = geometry_center_extent.find(np.key);
			if (ce_it == geometry_center_extent.end())
				continue;
			float diag = ce_it->second.second;
			const glm::mat4& W = node_world_engine[static_cast<size_t>(np.node_idx)];

			float area_proxy = std::max(diag * diag, 0.01f);
			float intensity_raw = strength * chroma * area_proxy * 0.08f;
			float intensity = std::clamp(intensity_raw, 0.25f, 50.0f);
			std::string mat_name = (mat_i < gltf.materials.size() && !gltf.materials[mat_i].name.empty())
			                       ? gltf.materials[mat_i].name : "Emissive " + std::to_string(emissive_light_count);
			const std::string& node_name = gltf.nodes[static_cast<size_t>(np.node_idx)].name;
			std::string individual_name = !node_name.empty() ? node_name : "light " + std::to_string(emissive_light_count);

			if (diag < EMISSIVE_CLUSTER_EXTENT) {
				const glm::vec3& center = ce_it->second.first;
				glm::vec3 world_pos = glm::vec3(W * glm::vec4(center, 1.f));
				if (!dedup.insert(quantize(world_pos)).second)
					continue;
				emissive_lights.push_back({
					.type = VeModel::ExtractedLightType::Point,
					.position = world_pos,
					.direction = glm::vec3(0.0f, 0.0f, -1.0f),
					.color = color_n,
					.intensity = intensity,
					.range = std::max(diag * 1.25f, 0.25f),
					.name = mat_name + ": " + individual_name
				});
				emissive_light_count++;
			} else {
				const tinygltf::Accessor* pos_acc = findPositionAccessor(np);
				if (!pos_acc) {
					const glm::vec3& center = ce_it->second.first;
					glm::vec3 world_pos = glm::vec3(W * glm::vec4(center, 1.f));
					if (!dedup.insert(quantize(world_pos)).second)
						continue;
					emissive_lights.push_back({
						.type = VeModel::ExtractedLightType::Point,
						.position = world_pos,
						.direction = glm::vec3(0.0f, 0.0f, -1.0f),
						.color = color_n,
						.intensity = intensity,
						.range = std::max(diag * 1.25f, 0.25f),
						.name = mat_name + ": " + individual_name
					});
					emissive_light_count++;
					continue;
				}
				const auto& bv = gltf.bufferViews[static_cast<size_t>(pos_acc->bufferView)];
				const auto& buf = gltf.buffers[static_cast<size_t>(bv.buffer)];
				const uint8_t* data = buf.data.data() + bv.byteOffset + pos_acc->byteOffset;
				int sv = pos_acc->ByteStride(bv);
				size_t stride = static_cast<size_t>(sv > 0 ? sv : 12);

				std::vector<glm::vec3> positions(pos_acc->count);
				for (size_t vi = 0; vi < pos_acc->count; vi++) {
					const float* fp = reinterpret_cast<const float*>(data + vi * stride);
					positions[vi] = {fp[0], -fp[2], fp[1]};  // Y-up to Z-up
				}

				std::vector<glm::vec3> centroids = clusterVertices(positions, EMISSIVE_CLUSTER_GAP, EMISSIVE_CLUSTER_EXTENT);

				for (size_t ci = 0; ci < centroids.size(); ci++) {
					glm::vec3 world_pos = glm::vec3(W * glm::vec4(centroids[ci], 1.f));
					if (!dedup.insert(quantize(world_pos)).second) continue;
					emissive_lights.push_back({
						.type = VeModel::ExtractedLightType::Point,
						.position = world_pos,
						.direction = glm::vec3(0.0f, 0.0f, -1.0f),
						.color = color_n,
						.intensity = intensity,
						.range = std::max(diag * 1.25f, 0.25f),
						.name = mat_name + ": " + individual_name + " [" + std::to_string(ci) + "]"
					});
					emissive_light_count++;
				}
			}
			if (emissive_light_count >= ve::MAX_LIGHTS - 1)
				VE_LOGW("Reached maximum light count while extracting emissive lights from model; some lights may be missing");
		}
	}
	return emissive_lights;
}

//----------------------------------
// VeModel implementation
//----------------------------------

// flip_tex_coord_v: when true, materials use flipped V for tex coords.
// Required for some gltf exporters.
std::unique_ptr<VeModel> VeModel::load(VeResourceManager& resource_manager,
                                       const std::filesystem::path& model_path,
                                       VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout,
                                       bool extract_lights,
                                       bool flip_tex_coord_v) {
	auto model = std::make_unique<VeModel>();
	model->loadFromGltf(model_path, resource_manager, pool, material_layout, extract_lights, flip_tex_coord_v);
	return model;
}

VeModel::VeModel() = default;

VeModel::~VeModel() = default;

void VeModel::loadFromGltf(const std::filesystem::path& model_path, VeResourceManager& resource_manager,
                           VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout,
                           bool extract_lights, bool flip_tex_coord_v) {
	tinygltf::Model gltf;
	tinygltf::TinyGLTF loader;
	// Accept KTX2/other formats as-is: we load textures via VeTexture from URI, not tinygltf's decoded data
	loader.SetImagesAsIs(true);
	tinygltf::LoadImageDataOption load_opt;
	load_opt.as_is = true;
	load_opt.preserve_channels = false;
	loader.SetImageLoader(LoadImageDataVeModel, &load_opt);
	std::string err, warn;
	bool ret = false;
	std::string path_ext = model_path.extension().string();
	std::transform(path_ext.begin(), path_ext.end(), path_ext.begin(), ::tolower);
	if (path_ext == ".glb") {
		ret = loader.LoadBinaryFromFile(&gltf, &err, &warn, model_path.string());
	} else if (path_ext == ".gltf") {
		ret = loader.LoadASCIIFromFile(&gltf, &err, &warn, model_path.string());
	}
	else {
		VE_LOGE("Unsupported file extension for glTF model: " << model_path);
		assert(false);
		return;
	}
	if (!ret) {
		VE_LOGE("Failed to load glTF: " << err);
		assert(false);
		return;
	}
	if (!warn.empty())
		VE_LOGW("glTF warning: " << warn);

	// Blender exports emissive in different units; scale down when generator is Blender (see glTF issue #2473).
	float emissive_scale = 1.0f;
	std::string generator_lower = gltf.asset.generator;
	std::transform(generator_lower.begin(), generator_lower.end(), generator_lower.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (generator_lower.find("blender") != std::string::npos) {
		emissive_scale = 1.0f / BLENDER_EMISSIVE_FACTOR;
		VE_LOGI("Blender generator detected, applying emissive scale " << emissive_scale);
	}

	// Parse material alpha props, factors, and texture paths from glTF
	std::vector<MaterialAlphaProps> material_alpha_props;
	std::vector<MaterialFactors> material_factors;
	std::vector<std::filesystem::path> albedo_paths, normal_paths, metallic_roughness_paths, occlusion_paths, emissive_paths;
	std::filesystem::path model_dir = model_path.parent_path();
	std::string model_path_str = model_path.lexically_normal().generic_string();
	bool has_textured_materials = false;

	// Register embedded images (glb) in the texture cache so VeTexture can load from memory
	for (size_t i = 0; i < gltf.images.size(); i++) {
		auto& img = gltf.images[i];
		if (!img.uri.empty() || img.image.empty())
			continue;
		std::string key = "@embedded:" + model_path_str + "::img_" + std::to_string(i);
		bool is_ktx = isKtxMagic(img.image.data(), img.image.size());
		VeTexture::registerEmbedded(key, {std::move(img.image),
		                                  is_ktx ? 0u : static_cast<uint32_t>(img.width),
		                                  is_ktx ? 0u : static_cast<uint32_t>(img.height), is_ktx});
	}

	const std::filesystem::path default_albedo = model_dir / "default_albedo.png";
	const std::filesystem::path default_normal = model_dir / "default_normal.png";
	const std::filesystem::path default_metallic_roughness = model_dir / "default_metallic_roughness.png";
	const std::filesystem::path default_occlusion = model_dir / "default_occlusion.png";
	const std::filesystem::path default_emissive = model_dir / "default_emissive.png";

	if (!gltf.materials.empty()) {
		for (const auto& mat : gltf.materials) {
			if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
			    mat.normalTexture.index >= 0 ||
			    mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
				has_textured_materials = true;
			}
			// KHR_materials_pbrSpecularGlossiness: diffuseTexture as albedo when baseColorTexture missing
			auto it_sg = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
			if (it_sg != mat.extensions.end()) {
				const tinygltf::Value& extension = it_sg->second;
				if (extension.Has("diffuseTexture") && extension.Get("diffuseTexture").Has("index")) {
					has_textured_materials = true;
				}
			}
			MaterialAlphaProps props;
			if (mat.alphaMode == "BLEND")
				props.alpha_mode = AlphaMode::BLEND;
			else if (mat.alphaMode == "MASK")
				props.alpha_mode = AlphaMode::MASK;
			else
				props.alpha_mode = AlphaMode::ALPHA_OPAQUE;
			props.alpha_cutoff = static_cast<float>(mat.alphaCutoff);
			props.double_sided = mat.doubleSided;
			material_alpha_props.push_back(props);

			// PBR factors from glTF
			MaterialFactors factors;
			if (mat.pbrMetallicRoughness.baseColorFactor.size() >= 4) {
				factors.base_color_factor = glm::vec4(
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]),
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3]));
			} else if (mat.pbrMetallicRoughness.baseColorFactor.size() >= 3) {
				factors.base_color_factor = glm::vec4(
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
					static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]), 1.0f);
			}
			factors.metallic_factor = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
			factors.roughness_factor = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
			if (mat.emissiveFactor.size() >= 3) {
				factors.emissive_factor = glm::vec3(
					static_cast<float>(mat.emissiveFactor[0]),
					static_cast<float>(mat.emissiveFactor[1]),
					static_cast<float>(mat.emissiveFactor[2]));
				factors.emissive_factor *= emissive_scale;
			}
			auto it_es = mat.extensions.find("KHR_materials_emissive_strength");
			if (it_es != mat.extensions.end() && it_es->second.Has("emissiveStrength") && it_es->second.Get("emissiveStrength").IsNumber()) {
				factors.emissive_strength = static_cast<float>(it_es->second.Get("emissiveStrength").Get<double>());
			} else if (glm::length(factors.emissive_factor) > 0.1f) {
				// Material has significant emissive color but no explicit strength extension.
				// Use glTF spec default (1.0) so the surface actually glows.
				factors.emissive_strength = 1.0f;
			}
			auto it_tr = mat.extensions.find("KHR_materials_transmission");
			if (it_tr != mat.extensions.end() && it_tr->second.Has("transmissionFactor") && it_tr->second.Get("transmissionFactor").IsNumber()) {
				factors.transmission_factor = static_cast<float>(it_tr->second.Get("transmissionFactor").Get<double>());
			}
			auto it_ior = mat.extensions.find("KHR_materials_ior");
			if (it_ior != mat.extensions.end() && it_ior->second.Has("ior") && it_ior->second.Get("ior").IsNumber()) {
				double ior_val = it_ior->second.Get("ior").Get<double>();
				// glTF allows 0 for legacy specular-glossiness mode; we treat as default 1.5
				factors.ior = (ior_val >= 1.0) ? static_cast<float>(ior_val) : 1.5f;
			}
			// KHR_materials_pbrSpecularGlossiness: diffuseFactor, glossinessFactor, specularFactor (when present, apply to factors)
			if (it_sg != mat.extensions.end()) {
				const tinygltf::Value& ext = it_sg->second;
				if (ext.Has("diffuseFactor") && ext.Get("diffuseFactor").IsArray()) {
					const auto& df = ext.Get("diffuseFactor");
					if (df.Size() >= 4) {
						factors.base_color_factor = glm::vec4(
							static_cast<float>(df.Get(0).Get<double>()),
							static_cast<float>(df.Get(1).Get<double>()),
							static_cast<float>(df.Get(2).Get<double>()),
							static_cast<float>(df.Get(3).Get<double>()));
					} else if (df.Size() >= 3) {
						factors.base_color_factor = glm::vec4(
							static_cast<float>(df.Get(0).Get<double>()),
							static_cast<float>(df.Get(1).Get<double>()),
							static_cast<float>(df.Get(2).Get<double>()), 1.0f);
					}
				}
				if (ext.Has("glossinessFactor") && ext.Get("glossinessFactor").IsNumber()) {
					float glossiness = static_cast<float>(ext.Get("glossinessFactor").Get<double>());
					factors.roughness_factor = std::clamp(1.0f - glossiness, 0.0f, 1.0f);
				}
				if (ext.Has("specularFactor") && ext.Get("specularFactor").IsArray()) {
					const auto& sf = ext.Get("specularFactor");
					if (sf.Size() >= 3) {
						factors.specular_factor.x = static_cast<float>(sf.Get(0).Get<double>());
						factors.specular_factor.y = static_cast<float>(sf.Get(1).Get<double>());
						factors.specular_factor.z = static_cast<float>(sf.Get(2).Get<double>());
					}
				} else {
					// Spec-gloss default when extension present but no specularFactor is [1,1,1]
					factors.specular_factor = glm::vec3(1.0f, 1.0f, 1.0f);
				}
			} else {
				// No spec-gloss: use IOR-derived F0 when IOR set, else default 0.04
				float f0 = ((factors.ior - 1.0f) / (factors.ior + 1.0f)) * ((factors.ior - 1.0f) / (factors.ior + 1.0f));
				factors.specular_factor = glm::vec3(f0, f0, f0);
			}
			// Glass/liquid heuristics from material name (liquid tweaks applied to factors; glass flag reserved for future)
			std::string mat_name_lower = mat.name.empty() ? "" : mat.name;
			std::transform(mat_name_lower.begin(), mat_name_lower.end(), mat_name_lower.begin(),
			               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			bool name_liquid = (mat_name_lower.find("beer") != std::string::npos || mat_name_lower.find("wine") != std::string::npos || mat_name_lower.find("liquid") != std::string::npos);
			if (name_liquid) {
				factors.base_color_factor.x = std::clamp(factors.base_color_factor.x * 1.4f, 0.0f, 4.0f);
				factors.base_color_factor.y = std::clamp(factors.base_color_factor.y * 1.4f, 0.0f, 4.0f);
				factors.base_color_factor.z = std::clamp(factors.base_color_factor.z * 1.4f, 0.0f, 4.0f);
				factors.roughness_factor = std::clamp(factors.roughness_factor * 0.8f, 0.0f, 1.0f);
				factors.base_color_factor.w = std::clamp(factors.base_color_factor.w * 1.2f, 0.15f, 1.0f);
			}
			// Glass/window materials without KHR_materials_transmission (e.g. forge_windows, frosted_glass) need transparent pass
			bool name_glass = (mat_name_lower.find("glass") != std::string::npos || mat_name_lower.find("window") != std::string::npos || mat_name_lower.find("frosted") != std::string::npos);
			bool name_bottle = (mat_name_lower.find("bottle") != std::string::npos);
			if (name_glass && factors.transmission_factor <= 0.0f) {
				factors.transmission_factor = 0.7f;
			}
			// Glass/windows: cap transmission and minimum base alpha (keep wine glasses / windows visible)
			// TODO: Make glass look
			if (name_glass) {
				if (factors.transmission_factor > 0.75f)
					factors.transmission_factor = 0.75f;
				if (factors.base_color_factor.w < 0.4f)
					factors.base_color_factor.w = 0.4f;
			}
			// Bottles: stricter so wine bottles stay clearly visible (glass body often exports very transparent)
			if (name_bottle) {
				if (factors.transmission_factor > 0.85f)
					factors.transmission_factor = 0.85f;
				if (factors.base_color_factor.w < 0.70f)
					factors.base_color_factor.w = 0.70f;
			}
			// glTF often exports alphaMode BLEND for all materials (e.g. Bistro "BLENDSHADER" materials). Treat as opaque unless there is real transparency/transmission.
			bool name_implies_transparency = name_glass || name_bottle ||
				(mat_name_lower.find("leaf") != std::string::npos) ||
				(mat_name_lower.find("foliage") != std::string::npos) ||
				(mat_name_lower.find("vine") != std::string::npos) ||
				(mat_name_lower.find("curtain") != std::string::npos) ||
				(mat_name_lower.find("decal") != std::string::npos);
			if (material_alpha_props.back().alpha_mode == AlphaMode::BLEND &&
				factors.transmission_factor <= 0.0f &&
				factors.base_color_factor.w >= 0.99f &&
				!name_implies_transparency) {
				material_alpha_props.back().alpha_mode = AlphaMode::ALPHA_OPAQUE;
			}
			material_factors.push_back(factors);

			// Albedo: baseColorTexture or KHR_materials_pbrSpecularGlossiness.diffuseTexture
			if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
				albedo_paths.push_back(resolveTexturePath(gltf,
					static_cast<size_t>(mat.pbrMetallicRoughness.baseColorTexture.index), model_dir, model_path_str, default_albedo));
			} else if (it_sg != mat.extensions.end()) {
				const tinygltf::Value& ext = it_sg->second;
				if (ext.Has("diffuseTexture") && ext.Get("diffuseTexture").Has("index")) {
					int tex_idx = ext.Get("diffuseTexture").Get("index").Get<int>();
					albedo_paths.push_back(resolveTexturePath(gltf, static_cast<size_t>(tex_idx), model_dir, model_path_str, default_albedo));
				} else {
					albedo_paths.push_back(default_albedo);
				}
			} else {
				albedo_paths.push_back(default_albedo);
			}
			// Normal
			if (mat.normalTexture.index >= 0) {
				normal_paths.push_back(resolveTexturePath(gltf, static_cast<size_t>(mat.normalTexture.index), model_dir, model_path_str, default_normal));
			} else {
				normal_paths.push_back(default_normal);
			}
			// Metallic-roughness: metallicRoughnessTexture or specularGlossinessTexture as stand-in (alpha = roughness)
			if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {				metallic_roughness_paths.push_back(resolveTexturePath(gltf,
					static_cast<size_t>(mat.pbrMetallicRoughness.metallicRoughnessTexture.index), model_dir, model_path_str, default_metallic_roughness));
			} else if (it_sg != mat.extensions.end()) {
				const tinygltf::Value& ext = it_sg->second;
				if (ext.Has("specularGlossinessTexture") && ext.Get("specularGlossinessTexture").Has("index")) {
					int tex_idx = ext.Get("specularGlossinessTexture").Get("index").Get<int>();
					metallic_roughness_paths.push_back(resolveTexturePath(gltf, static_cast<size_t>(tex_idx), model_dir, model_path_str, default_metallic_roughness));
					material_alpha_props.back().use_spec_gloss_texture = true;
				} else {
					metallic_roughness_paths.push_back(default_metallic_roughness);
				}
			} else {
				metallic_roughness_paths.push_back(default_metallic_roughness);
			}
			// Occlusion
			if (mat.occlusionTexture.index >= 0) {
				occlusion_paths.push_back(resolveTexturePath(gltf, static_cast<size_t>(mat.occlusionTexture.index), model_dir, model_path_str, default_occlusion));
			} else {
				occlusion_paths.push_back(default_occlusion);
			}
			// Emissive
			if (mat.emissiveTexture.index >= 0) {
				emissive_paths.push_back(resolveTexturePath(gltf, static_cast<size_t>(mat.emissiveTexture.index), model_dir, model_path_str, default_emissive));
			} else {
				emissive_paths.push_back(default_emissive);
			}

			// Filename-based fallbacks when glTF did not reference a texture
			std::filesystem::path& albedo = albedo_paths.back();
			std::filesystem::path& normal = normal_paths.back();
			std::filesystem::path& metallic_roughness = metallic_roughness_paths.back();
			if (albedo == default_albedo && normal != default_normal) {
				std::filesystem::path p = tryDerivePath(normal,
					{"_ddna", "_n", "_normal"},
					{"_d", "_c", "_diffuse", "_basecolor", "_albedo"});
				if (!p.empty()) albedo = p;
			}
			if (normal == default_normal && albedo != default_albedo) {
				std::filesystem::path p = tryDerivePath(albedo,
					{"_d", "_c", "_diffuse", "_basecolor", "_albedo"},
					{"_n", "_normal", "_ddna"});
				if (!p.empty()) normal = p;
			}
			if (metallic_roughness == default_metallic_roughness && albedo != default_albedo) {
				std::filesystem::path p = tryDerivePath(albedo,
					{"_d", "_c", "_diffuse", "_basecolor", "_albedo"},
					{"_mr", "_metallic", "_roughness", "_specular"});
				if (!p.empty()) metallic_roughness = p;
			} else 			if (metallic_roughness == default_metallic_roughness && normal != default_normal) {
				std::filesystem::path p = tryDerivePath(normal,
					{"_ddna", "_n", "_normal"},
					{"_mr", "_metallic", "_roughness", "_specular"});
				if (!p.empty()) metallic_roughness = p;
			}
			// Secondary heuristic: if albedo still default, scan glTF images by URI for basecolor/albedo/diffuse + material name match
			if (albedo == default_albedo && !gltf.images.empty()) {
				std::string mat_name_lower2 = mat.name;
				if (!mat_name_lower2.empty()) {
					std::transform(mat_name_lower2.begin(), mat_name_lower2.end(), mat_name_lower2.begin(),
					               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				}
				for (size_t img_idx = 0; img_idx < gltf.images.size(); img_idx++) {
					const auto& image = gltf.images[img_idx];
					if (image.uri.empty()) continue;
					std::string uri_lower = image.uri;
					std::transform(uri_lower.begin(), uri_lower.end(), uri_lower.begin(),
					               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					bool looks_base = (uri_lower.find("basecolor") != std::string::npos || uri_lower.find("albedo") != std::string::npos || uri_lower.find("diffuse") != std::string::npos);
					if (!looks_base) continue;
					bool name_match = (!mat_name_lower2.empty() && uri_lower.find(mat_name_lower2) != std::string::npos);
					if (!name_match) {
						size_t us = uri_lower.find('_');
						if (us != std::string::npos && !mat_name_lower2.empty()) {
							std::string prefix = uri_lower.substr(0, us);
							name_match = (mat_name_lower2.find(prefix) != std::string::npos);
						}
					}
					if (!name_match) continue;
					albedo = model_dir / image.uri;
					break;
				}
			}
		}
	} else {
		// No materials in glTF: use single default material
		material_alpha_props.push_back(MaterialAlphaProps{});
		material_factors.push_back(MaterialFactors{});
		albedo_paths.push_back(model_dir / "default_albedo.png");
		normal_paths.push_back(model_dir / "default_normal.png");
		metallic_roughness_paths.push_back(model_dir / "default_metallic_roughness.png");
		occlusion_paths.push_back(model_dir / "default_occlusion.png");
		emissive_paths.push_back(model_dir / "default_emissive.png");
	}

	// Create VeMaterial resources
	VeDescriptorPool* mat_pool = has_textured_materials ? pool : nullptr;
	VeDescriptorSetLayout* mat_layout = has_textured_materials ? material_layout : nullptr;
	for (size_t i = 0; i < albedo_paths.size(); i++) {
		std::string mat_id = model_path.generic_string() + "::material_" + std::to_string(i);
		auto mat_handle = resource_manager.createMaterial(mat_id, albedo_paths[i], normal_paths[i],
		                                                  metallic_roughness_paths[i], occlusion_paths[i], emissive_paths[i],
		                                                  material_alpha_props[i], material_factors[i], mat_pool, mat_layout,
		                                                  flip_tex_coord_v);
		if (!mat_handle.isValid()) {
			VE_LOGE("Failed to create material " << i);
			continue;
		}
		m_material_handles.push_back(std::move(mat_handle));
	}
	VeTexture::clearEmbeddedCache();

	std::vector<int> root_nodes;
	if (!gltf.scenes.empty() && gltf.defaultScene >= 0 && gltf.defaultScene < static_cast<int>(gltf.scenes.size())) {
		root_nodes = gltf.scenes[static_cast<size_t>(gltf.defaultScene)].nodes;
	}
	if (root_nodes.empty() && !gltf.nodes.empty()) {
		root_nodes.push_back(0);
	}

	// Compute node world matrices when any light extraction is needed
	std::vector<glm::mat4> node_world_engine;
	PosDedup light_pos_dedup;
	if (extract_lights) {
		node_world_engine = computeNodeWorldMatrices(gltf, root_nodes);
	}

	// Punctual lights (KHR_lights_punctual extension)
	if (extract_lights) {
		m_punctual_lights = extractPunctualLights(gltf, node_world_engine);
		for (const auto& L : m_punctual_lights)
			light_pos_dedup.insert(quantize(L.position));
	}

	// Geometry key for mesh deduplication: same geometry+material shares one VeMesh.
	auto geometryKey = [](const tinygltf::Primitive& primitive, size_t material_index) -> std::string {
		std::string key = "mat_" + std::to_string(material_index) + "_idx_" + std::to_string(primitive.indices);
		for (const auto& [attr_name, accessor_idx] : primitive.attributes) {
			key += "_" + attr_name + "_" + std::to_string(accessor_idx);
		}
		return key;
	};
	std::unordered_map<std::string, ResourceHandle<VeMesh>> geometry_mesh_cache;
	GeometryCenterExtent geometry_center_extent;
	std::vector<NodePrim> node_primitives;

	// Create a VeMesh for a gltf primitive. Converts glTF Y-up to engine Z-up via (x,-z,y) to preserve handedness.
	// Requires POSITION and NORMAL. If primitive.indices < 0, generates sequential indices.
	// When TANGENT is missing, MikkTSpace generates tangents before vertex deduplication.
	// If out_center_extent is non-null, writes (center, diagonal extent) from out_vertices.
	auto createPrimitiveMesh = [&](const tinygltf::Primitive& primitive, const tinygltf::Model& m,
	                              const std::string& mesh_id,
	                              std::pair<glm::vec3, float>* out_center_extent = nullptr) -> ResourceHandle<VeMesh> {
		std::vector<VeMesh::Vertex> vertices;
		std::vector<uint32_t> indices;

		const tinygltf::Accessor& pos_accessor = m.accessors[static_cast<size_t>(primitive.attributes.at("POSITION"))];
		const tinygltf::BufferView& pos_bv = m.bufferViews[static_cast<size_t>(pos_accessor.bufferView)];
		const tinygltf::Buffer& pos_buf = m.buffers[static_cast<size_t>(pos_bv.buffer)];

		const tinygltf::Accessor* index_accessor = (primitive.indices >= 0) ? &m.accessors[static_cast<size_t>(primitive.indices)] : nullptr;
		const tinygltf::BufferView* index_bv = index_accessor ? &m.bufferViews[static_cast<size_t>(index_accessor->bufferView)] : nullptr;
		const tinygltf::Buffer* index_buf = index_bv ? &m.buffers[static_cast<size_t>(index_bv->buffer)] : nullptr;

		const tinygltf::Accessor& normal_accessor = m.accessors[static_cast<size_t>(primitive.attributes.at("NORMAL"))];
		const tinygltf::BufferView& normal_bv = m.bufferViews[static_cast<size_t>(normal_accessor.bufferView)];
		const tinygltf::Buffer& normal_buf = m.buffers[static_cast<size_t>(normal_bv.buffer)];

		bool has_tangents = primitive.attributes.find("TANGENT") != primitive.attributes.end();
		const tinygltf::Accessor* tangent_acc = has_tangents ? &m.accessors[static_cast<size_t>(primitive.attributes.at("TANGENT"))] : nullptr;
		const tinygltf::BufferView* tangent_bv = has_tangents ? &m.bufferViews[static_cast<size_t>(tangent_acc->bufferView)] : nullptr;
		const tinygltf::Buffer* tangent_buf = has_tangents ? &m.buffers[static_cast<size_t>(tangent_bv->buffer)] : nullptr;

		bool has_tex_coords = primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end();
		const tinygltf::Accessor* tex_acc = has_tex_coords ? &m.accessors[static_cast<size_t>(primitive.attributes.at("TEXCOORD_0"))] : nullptr;
		const tinygltf::BufferView* tex_bv = has_tex_coords ? &m.bufferViews[static_cast<size_t>(tex_acc->bufferView)] : nullptr;
		const tinygltf::Buffer* tex_buf = has_tex_coords ? &m.buffers[static_cast<size_t>(tex_bv->buffer)] : nullptr;

		// Strides
		int pos_stride_val = pos_accessor.ByteStride(pos_bv);
		const size_t pos_stride = static_cast<size_t>(pos_stride_val > 0 ? pos_stride_val : 12);
		int normal_stride_val = normal_accessor.ByteStride(normal_bv);
		const size_t normal_stride = static_cast<size_t>(normal_stride_val > 0 ? normal_stride_val : 12);
		size_t tex_stride = 8;
		if (has_tex_coords) {
			int ts = tex_acc->ByteStride(*tex_bv);
			if (ts > 0) tex_stride = static_cast<size_t>(ts);
			else if (tex_acc->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) tex_stride = 2u;
			else if (tex_acc->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) tex_stride = 4u;
		}
		int tangent_stride_val = has_tangents ? tangent_acc->ByteStride(*tangent_bv) : 0;
		const size_t tangent_stride = static_cast<size_t>(tangent_stride_val > 0 ? tangent_stride_val : 16);
		size_t index_stride = 2;
		if (index_accessor && index_bv) {
			int istr = index_accessor->ByteStride(*index_bv);
			if (istr > 0) index_stride = static_cast<size_t>(istr);
			else {
				switch (index_accessor->componentType) {
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: index_stride = 1; break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: index_stride = 2; break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: index_stride = 4; break;
					default: index_stride = 2; break;
				}
			}
		}

		// Stage 1: build full vertex array (one entry per position, no dedup yet) and raw indices
		vertices.reserve(static_cast<size_t>(pos_accessor.count));
		for (size_t i = 0; i < pos_accessor.count; i++) {
			VeMesh::Vertex vertex{};
			const float* pos = reinterpret_cast<const float*>(&pos_buf.data[pos_bv.byteOffset + pos_accessor.byteOffset + i * pos_stride]);
			vertex.pos = {pos[0], -pos[2], pos[1]};
			const float* normal = reinterpret_cast<const float*>(&normal_buf.data[normal_bv.byteOffset + normal_accessor.byteOffset + i * normal_stride]);
			vertex.normal = {normal[0], -normal[2], normal[1]};
			if (has_tex_coords && tex_stride > 0) {
				const uint8_t* tex_base = &tex_buf->data[tex_bv->byteOffset + tex_acc->byteOffset + i * tex_stride];
				switch (tex_acc->componentType) {
					case TINYGLTF_COMPONENT_TYPE_FLOAT: {
						const float* tc = reinterpret_cast<const float*>(tex_base);
						vertex.tex_coord = {tc[0], tc[1]};
						break;
					}
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
						uint8_t u = tex_base[0], v = tex_base[1];
						vertex.tex_coord = tex_acc->normalized ? glm::vec2{u / 255.0f, v / 255.0f} : glm::vec2{static_cast<float>(u), static_cast<float>(v)};
						break;
					}
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
						const uint16_t* tc = reinterpret_cast<const uint16_t*>(tex_base);
						vertex.tex_coord = tex_acc->normalized ? glm::vec2{tc[0] / 65535.0f, tc[1] / 65535.0f} : glm::vec2{static_cast<float>(tc[0]), static_cast<float>(tc[1])};
						break;
					}
					default: vertex.tex_coord = {0, 0}; break;
				}
			} else {
				vertex.tex_coord = {0, 0};
			}
			if (has_tangents && tangent_stride > 0) {
				const float* t = reinterpret_cast<const float*>(&tangent_buf->data[tangent_bv->byteOffset + tangent_acc->byteOffset + i * tangent_stride]);
				glm::vec3 T(t[0], -t[2], t[1]);
				glm::vec3 N = vertex.normal;
				if (glm::dot(T, T) > 0.0f) {
					T = glm::normalize(T);
					T = T - N * glm::dot(N, T);
					if (glm::dot(T, T) > 0.0f)
						T = glm::normalize(T);
					else
						T = glm::vec3(1.0f, 0.0f, 0.0f);
				} else {
					T = glm::vec3(1.0f, 0.0f, 0.0f);
				}
				float w = (t[3] >= 0.0f) ? 1.0f : -1.0f;
				vertex.tangent = glm::vec4(T, w);
			} else {
				vertex.tangent = {0, 0, 0, 0};
			}
			vertices.push_back(vertex);
		}

		const size_t vertex_count = vertices.size();
		if (index_accessor && index_buf) {
			const unsigned char* index_data = &index_buf->data[index_bv->byteOffset + index_accessor->byteOffset];
			for (size_t i = 0; i < static_cast<size_t>(index_accessor->count); i++) {
				uint32_t accessor_index = 0;
				const unsigned char* elem = index_data + i * index_stride;
				switch (index_accessor->componentType) {
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: accessor_index = *reinterpret_cast<const uint8_t*>(elem); break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: accessor_index = *reinterpret_cast<const uint16_t*>(elem); break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: accessor_index = *reinterpret_cast<const uint32_t*>(elem); break;
					default: assert(false);
				}
				if (accessor_index >= vertex_count) {
					VE_LOGW("glTF index out of range: " << accessor_index << " >= " << vertex_count << ", clamping");
					accessor_index = vertex_count > 0 ? static_cast<uint32_t>(vertex_count - 1) : 0;
				}
				indices.push_back(accessor_index);
			}
		} else {
			for (uint32_t j = 0; j < vertex_count; j++)
				indices.push_back(j);
		}

		// Stage 2: generate tangents via MikkTSpace when glTF has no TANGENT
		if (!has_tangents && has_tex_coords && indices.size() >= 3 && (indices.size() % 3) == 0) {
			SMikkTSpaceInterface iface{};
			iface.m_getNumFaces = mikkGetNumFaces;
			iface.m_getNumVerticesOfFace = mikkGetNumVerticesOfFace;
			iface.m_getPosition = mikkGetPosition;
			iface.m_getNormal = mikkGetNormal;
			iface.m_getTexCoord = mikkGetTexCoord;
			iface.m_setTSpaceBasic = mikkSetTSpaceBasic;
			iface.m_setTSpace = nullptr;
			SMikkTSpaceContext ctx{};
			ctx.m_pInterface = &iface;
			MikkTSpaceBridge bridge{&vertices, &indices};
			ctx.m_pUserData = &bridge;
			if (genTangSpaceDefault(&ctx)) {
				// tangents filled in vertices
				VE_LOGI("MikkTSpace tangent generation successful for mesh " << mesh_id);
			} else {
				VE_LOGW("MikkTSpace tangent generation failed for mesh " << mesh_id);
			}
		}

		// Stage 3: deduplicate vertices and remap indices
		std::unordered_map<VeMesh::Vertex, uint32_t> unique_vertices;
		std::vector<VeMesh::Vertex> out_vertices;
		std::vector<uint32_t> out_indices;
		out_vertices.reserve(vertices.size());
		out_indices.reserve(indices.size());
		for (uint32_t idx : indices) {
			const VeMesh::Vertex& v = vertices[idx];
			auto [it, inserted] = unique_vertices.try_emplace(v, static_cast<uint32_t>(out_vertices.size()));
			if (inserted)
				out_vertices.push_back(v);
			out_indices.push_back(it->second);
		}

		if (out_center_extent && !out_vertices.empty()) {
			glm::vec3 mn(out_vertices[0].pos), mx(out_vertices[0].pos);
			glm::vec3 sum(0.f);
			for (const auto& vert : out_vertices) {
				mn = glm::min(mn, vert.pos);
				mx = glm::max(mx, vert.pos);
				sum += vert.pos;
			}
			out_center_extent->first = sum / static_cast<float>(out_vertices.size());
			out_center_extent->second = glm::length(mx - mn);
		}

		// Stage 4a: optimize base mesh for vertex cache and overdraw
		meshopt_optimizeVertexCache(out_indices.data(), out_indices.data(),
		                            out_indices.size(), out_vertices.size());
		meshopt_optimizeOverdraw(out_indices.data(), out_indices.data(),
		                         out_indices.size(),
		                         &out_vertices[0].pos.x, out_vertices.size(),
		                         sizeof(VeMesh::Vertex), 1.05f);

		// Stage 4b: generate LOD levels via progressive simplification
		std::vector<std::vector<uint32_t>> lod_indices;
		size_t base_index_count = out_indices.size();

		// Build attribute array (normals + UVs) for attribute-aware simplification
		// This prevents collapsing edges across UV seams and normal discontinuities
		const size_t attr_count = 5; // normal(3) + uv(2)
		std::vector<float> vertex_attributes(out_vertices.size() * attr_count);
		for (size_t v = 0; v < out_vertices.size(); v++) {
			vertex_attributes[v * attr_count + 0] = out_vertices[v].normal.x;
			vertex_attributes[v * attr_count + 1] = out_vertices[v].normal.y;
			vertex_attributes[v * attr_count + 2] = out_vertices[v].normal.z;
			vertex_attributes[v * attr_count + 3] = out_vertices[v].tex_coord.x;
			vertex_attributes[v * attr_count + 4] = out_vertices[v].tex_coord.y;
		}
		const float attribute_weights[attr_count] = {0.5f, 0.5f, 0.5f, 1.0f, 1.0f};

		for (uint32_t lod = 1; lod < ve::MAX_LOD_LEVELS; lod++) {
			size_t target_count = static_cast<size_t>(
				static_cast<float>(base_index_count) * ve::LOD_RATIOS[lod]);
			target_count = std::max(target_count, static_cast<size_t>(ve::LOD_MIN_TRIANGLES * 3));
			if (target_count >= base_index_count)
				break;

			// Always simplify from the base mesh to avoid accumulating error
			if (target_count >= out_indices.size())
				break;

			std::vector<uint32_t> simplified(out_indices.size());
			float result_error = 0.0f;
			size_t result_count = meshopt_simplifyWithAttributes(
				simplified.data(),
				out_indices.data(),
				out_indices.size(),
				&out_vertices[0].pos.x,
				out_vertices.size(),
				sizeof(VeMesh::Vertex),
				vertex_attributes.data(),
				sizeof(float) * attr_count,
				attribute_weights,
				attr_count,
				nullptr,
				target_count,
				ve::LOD_ERROR_THRESHOLD,
				meshopt_SimplifyLockBorder,
				&result_error
			);
			simplified.resize(result_count);

			// Stop if simplification couldn't meaningfully reduce from the previous LOD
			size_t prev_count = lod_indices.empty() ? base_index_count : lod_indices.back().size();
			if (result_count >= prev_count * 95 / 100)
				break;

			meshopt_optimizeVertexCache(simplified.data(), simplified.data(),
			                            simplified.size(), out_vertices.size());

			lod_indices.push_back(std::move(simplified));
		}

		if (lod_indices.empty())
			return resource_manager.createMesh(mesh_id, out_vertices, out_indices);
		return resource_manager.createMesh(mesh_id, out_vertices, out_indices, lod_indices);
	};

	// Recursively create nodes.
	// Each glTF node becomes a LoadedNode with transform + optional mesh/material.
	// A glTF node with multiple primitives produces child LoadedNodes for primitives 2+.
	std::function<void(int, int)> processNode = [&](int gltf_node_idx, int parent_our_idx) {
		const auto& node = gltf.nodes[static_cast<size_t>(gltf_node_idx)];
		NodeTRS trs = getNodeTRS(node);

		LoadedNode loaded{};
		loaded.id = s_next_node_id++;
		loaded.name = node.name;
		loaded.translation = trs.translation;
		loaded.rotation = trs.rotation;
		loaded.scale = trs.scale;

		int node_our_idx = static_cast<int>(m_nodes.size());
		uint32_t node_id = loaded.id;
		m_nodes.push_back(std::move(loaded));

		if (parent_our_idx >= 0) {
			m_parent_links.emplace_back(node_id, m_nodes[static_cast<size_t>(parent_our_idx)].id);
		} else {
			m_root_ids.insert(node_id);
		}

		// If node has mesh: assign first primitive to this node, rest as children
		if (node.mesh >= 0) {
			const auto& mesh = gltf.meshes[static_cast<size_t>(node.mesh)];
			for (size_t prim_idx = 0; prim_idx < mesh.primitives.size(); prim_idx++) {
				const auto& primitive = mesh.primitives[prim_idx];
				if (primitive.attributes.find("NORMAL") == primitive.attributes.end())
					continue;
				size_t mat_idx = (primitive.material >= 0 && static_cast<size_t>(primitive.material) < m_material_handles.size())
				                    ? static_cast<size_t>(primitive.material) : 0;
				std::string key = geometryKey(primitive, mat_idx);
				ResourceHandle<VeMesh> mesh_handle;
				auto cache_it = geometry_mesh_cache.find(key);
				if (cache_it != geometry_mesh_cache.end()) {
					mesh_handle = cache_it->second;
				} else {
					std::string mesh_id = model_path_str + "::" + key;
					mesh_handle = createPrimitiveMesh(primitive, gltf, mesh_id, &geometry_center_extent[key]);
					if (mesh_handle.isValid())
						geometry_mesh_cache[key] = mesh_handle;
				}
				node_primitives.push_back({gltf_node_idx, key, mat_idx});
				ResourceHandle<VeMaterial> mat_handle = (mat_idx < m_material_handles.size())
				                                           ? m_material_handles[mat_idx]
				                                           : ResourceHandle<VeMaterial>{};
				if (mat_handle.isValid()) {
					if (prim_idx == 0) {
						m_nodes[static_cast<size_t>(node_our_idx)].mesh = std::move(mesh_handle);
						m_nodes[static_cast<size_t>(node_our_idx)].material = std::move(mat_handle);
					} else {
						LoadedNode prim_node{};
						prim_node.id = s_next_node_id++;
						prim_node.mesh = std::move(mesh_handle);
						prim_node.material = m_material_handles[mat_idx];
						m_parent_links.emplace_back(prim_node.id, node_id);
						m_nodes.push_back(std::move(prim_node));
					}
				}
			}
		}

		for (int child_idx : node.children) {
			processNode(child_idx, node_our_idx);
		}
	};

	for (int root_idx : root_nodes) {
		processNode(root_idx, -1);
		if (m_root_id == 0 && !m_nodes.empty()) {
			m_root_id = m_nodes[0].id;
		}
	}
	if (!m_nodes.empty() && m_root_id == 0) {
		m_root_id = m_nodes[0].id;
	}

	// Emissive-as-lights extraction (after mesh processing so node_primitives is populated)
	if (extract_lights) {
		m_emissive_lights = extractEmissiveLights(gltf, node_world_engine,
			node_primitives, geometry_center_extent, material_factors, light_pos_dedup);
	}

	VE_LOGI("Loaded model " << model_path << " with " << m_nodes.size() << " nodes, "
	        << m_punctual_lights.size() << " punctual, " << m_emissive_lights.size() << " emissive lights");
}

void VeModel::addToScene(Registry& registry,
                         const glm::vec3& root_translation,
                         const glm::vec3& root_rotation,
                         const glm::vec3& root_scale) {
	// Wrapper entity for root transform
	Entity wrapper = registry.createGameObject();
	auto* wrapper_tc = registry.getComponent<TransformComponent>(wrapper);
	wrapper_tc->setTranslation(root_translation);
	wrapper_tc->setRotationEuler(root_rotation);
	wrapper_tc->setScale(root_scale);

	// Build lookup structures for world transform computation (used for dedup)
	std::unordered_map<uint32_t, uint32_t> parent_of;
	for (const auto& [child_id, parent_id] : m_parent_links)
		parent_of[child_id] = parent_id;
	std::unordered_map<uint32_t, size_t> id_to_index;
	for (size_t i = 0; i < m_nodes.size(); i++)
		id_to_index[m_nodes[i].id] = i;

	auto localMatrix = [](const LoadedNode& n) -> glm::mat4 {
		return glm::translate(glm::mat4(1.0f), n.translation)
			* glm::mat4_cast(n.rotation)
			* glm::scale(glm::mat4(1.0f), n.scale);
	};
	auto worldTransform = [&](uint32_t node_id) -> glm::mat4 {
		std::vector<uint32_t> chain;
		for (uint32_t cur = node_id; ; ) {
			chain.push_back(cur);
			auto it = parent_of.find(cur);
			if (it == parent_of.end()) break;
			cur = it->second;
		}
		glm::mat4 world(1.0f);
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			auto idx_it = id_to_index.find(*it);
			if (idx_it != id_to_index.end())
				world *= localMatrix(m_nodes[idx_it->second]);
		}
		return world;
	};

	// Dedup: same mesh+material at same world position → skip duplicate MeshComponent.
	// Uses mesh pointer (not vertex/index counts) to avoid false positives from different
	// geometry that happens to share counts.
	struct DedupKey {
		const void* mesh;
		const void* material;
		int32_t qx, qy, qz;
		bool operator==(const DedupKey&) const = default;
	};
	struct DedupHash {
		size_t operator()(const DedupKey& k) const {
			size_t h = std::hash<const void*>{}(k.mesh);
			h ^= std::hash<const void*>{}(k.material) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.qx) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.qy) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.qz) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};
	std::unordered_set<DedupKey, DedupHash> mesh_dedup;
	uint32_t dedup_count = 0;

	// Map LoadedNode IDs to new Entity IDs
	std::unordered_map<uint32_t, Entity> id_map;
	for (const auto& node : m_nodes) {
		Entity entity = registry.createEntity(node.name);
		id_map[node.id] = entity;

		// TransformComponent (every node has TRS)
		auto& tc = registry.addComponent<TransformComponent>(entity);
		tc.setTranslation(node.translation);
		tc.setRotation(node.rotation);
		tc.setScale(node.scale);

		// MeshComponent (only if node has valid mesh+material)
		if (node.mesh.isValid() && node.material.isValid()) {
			glm::vec3 pos(worldTransform(node.id)[3]);
			DedupKey key{
				node.mesh.get(), node.material.get(),
				static_cast<int32_t>(std::round(pos.x * 1000.0f)),
				static_cast<int32_t>(std::round(pos.y * 1000.0f)),
				static_cast<int32_t>(std::round(pos.z * 1000.0f))
			};
			if (mesh_dedup.insert(key).second) {
				auto& mc = registry.addComponent<MeshComponent>(entity, node.mesh, node.material);
				auto* mat = node.material.get();
				mc.has_texture = (mat && mat->hasDescriptorSet()) ? 1.0f : 0.0f;
			} else {
				dedup_count++;
			}
		}
	}
	if (dedup_count > 0)
		VE_LOGI("addToScene: skipped " << dedup_count << " duplicate mesh instances");
	m_nodes.clear();

	// Set up hierarchy from parent links
	for (const auto& [child_id, parent_id] : m_parent_links) {
		auto child_it = id_map.find(child_id);
		auto parent_it = id_map.find(parent_id);
		if (child_it != id_map.end() && parent_it != id_map.end()) {
			registry.setParent(child_it->second, parent_it->second);
		}
	}
	// Make all glTF roots children of the wrapper
	for (uint32_t root_id : m_root_ids) {
		auto it = id_map.find(root_id);
		if (it != id_map.end()) {
			registry.setParent(it->second, wrapper);
		}
	}

	// Extracted lights: place at world position (wrapper * L.position)
	const glm::mat4 wrapper_world = glm::translate(glm::mat4(1.0f), root_translation)
		* glm::mat4_cast(glm::quat_cast(glm::eulerAngleZYX(root_rotation.z, root_rotation.y, root_rotation.x)))
		* glm::scale(glm::mat4(1.0f), root_scale);
	constexpr float size = 0.1f;
	for (const ExtractedLight& L : m_emissive_lights) {
		Entity light = registry.createPointLight(L.intensity * EMISSIVE_LIGHT_INTENSITY_SCALE, size, L.color);
		registry.setName(light, L.name.empty() ? "Light (emissive)" : L.name);
		registry.setLightSource(light, LightSource::Emissive);
		auto* tc = registry.getComponent<TransformComponent>(light);
		tc->setTranslation(glm::vec3(wrapper_world * glm::vec4(L.position, 1.0f)));
		registry.setActive(light, false);  // default OFF (MAX_LIGHTS constraint)
	}
	for (const ExtractedLight& L : m_punctual_lights) {
		float scaled_intensity = L.intensity * KHR_PUNCTUAL_INTENSITY_SCALE;
		if (L.type == ExtractedLightType::Directional) {
			// Transform direction by wrapper rotation (ignore translation/scale for directions)
			glm::vec3 world_dir = glm::normalize(glm::mat3(wrapper_world) * L.direction);
			Entity light = registry.createDirectionalLight(scaled_intensity, L.color, world_dir);
			registry.setName(light, L.name.empty() ? "Light (directional)" : L.name);
			registry.setLightSource(light, LightSource::Punctual);
			registry.setActive(light, false);  // default OFF
		} else {
			Entity light = registry.createPointLight(scaled_intensity, size, L.color);
			registry.setName(light, L.name.empty() ? "Light (imported)" : L.name);
			registry.setLightSource(light, LightSource::Punctual);
			auto* tc = registry.getComponent<TransformComponent>(light);
			tc->setTranslation(glm::vec3(wrapper_world * glm::vec4(L.position, 1.0f)));
			auto* plc = registry.getComponent<PointLightComponent>(light);
			if (plc) plc->setRange(L.range);
			registry.setActive(light, false);  // default OFF
		}
	}
}

std::optional<VeModel::SingleMeshData> VeModel::loadSingleMesh(
	VeResourceManager& resource_manager,
	const std::filesystem::path& model_path,
	bool flip_tex_coord_v) {
	auto model = load(resource_manager, model_path.lexically_normal(), nullptr, nullptr, false, flip_tex_coord_v);
	for (const auto& node : model->m_nodes) {
		if (node.mesh.isValid() && node.material.isValid()) {
			return SingleMeshData{node.mesh, node.material};
		}
	}
	return std::nullopt;
}

} // namespace ve
