/* gltf_loader.cpp - generated from ve_model.cpp loader code */
#include "pch.hpp"
#include "resources/internal/gltf_loader.hpp"
#include "resources/ve_texture.hpp"
#include "resources/internal/loaded_asset_data.hpp"
#include "resources/internal/asset_upload.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_parallel.hpp"

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
#include <mikktspace.h>
#include <meshoptimizer.h>
#include "rendering/culling/meshlet_data.hpp"

#include <algorithm>
#include <fstream>
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

// Private cache key for a .glb-embedded image
static std::string makeEmbeddedKey(const std::string& model_path_str, int img_idx) {
	return "@embedded:" + model_path_str + "::img_" + std::to_string(img_idx);
}

// Resolve texture path from glTF image. External images use URI on disk; embedded
// images return their cache key. Empty path means no resolvable backing image.
static std::filesystem::path resolveTexturePath(const tinygltf::Model& gltf, size_t tex_idx,
                                                const std::filesystem::path& model_dir,
                                                const std::string& model_path_str,
                                                const EmbeddedImageCache& embedded) {
	int img_idx = getTextureImageIndex(gltf, tex_idx);
	if (img_idx < 0)
		return {};
	const auto& image = gltf.images[static_cast<size_t>(img_idx)];
	if (!image.uri.empty())
		return model_dir / image.uri;
	std::string emb_key = makeEmbeddedKey(model_path_str, img_idx);
	if (embedded.find(emb_key) != embedded.end())
		return std::filesystem::path(emb_key);
	return {};
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

// Byte size of a single glTF component (BYTE=1, SHORT=2, FLOAT=4, etc.)
static size_t gltfComponentSize(int componentType) {
	switch (componentType) {
		case TINYGLTF_COMPONENT_TYPE_BYTE:
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
		case TINYGLTF_COMPONENT_TYPE_SHORT:
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
		default: return 4;
	}
}

// Read a single float from any glTF component type, applying normalization when flagged.
static float readGltfComponent(const uint8_t* data, int componentType, bool normalized) {
	switch (componentType) {
		case TINYGLTF_COMPONENT_TYPE_FLOAT:
			return *reinterpret_cast<const float*>(data);
		case TINYGLTF_COMPONENT_TYPE_SHORT: {
			int16_t v = *reinterpret_cast<const int16_t*>(data);
			return normalized ? std::max(v / 32767.0f, -1.0f) : static_cast<float>(v);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
			uint16_t v = *reinterpret_cast<const uint16_t*>(data);
			return normalized ? v / 65535.0f : static_cast<float>(v);
		}
		case TINYGLTF_COMPONENT_TYPE_BYTE: {
			int8_t v = *reinterpret_cast<const int8_t*>(data);
			return normalized ? std::max(v / 127.0f, -1.0f) : static_cast<float>(v);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
			return normalized ? *data / 255.0f : static_cast<float>(*data);
		default:
			return *reinterpret_cast<const float*>(data);
	}
}

// Intermediate result from parsing a single glTF material.
// Groups all data needed to create a VeMaterial resource.
struct ParsedMaterial {
	MaterialAlphaProps alpha_props;
	MaterialFactors factors;
	MaterialUvTransforms uv_transforms;
	std::filesystem::path albedo_path, normal_path, metallic_roughness_path,
	                      occlusion_path, emissive_path,
	                      specular_path, specular_color_path;
	bool has_textures = false;
};

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

// Read all float values from a glTF accessor into a flat vector, applying any sparse overlay.
static std::vector<float> readAccessorFloats(const tinygltf::Model& gltf, int accessor_idx, int expected_components) {
	const auto& acc = gltf.accessors[static_cast<size_t>(accessor_idx)];
	const size_t comp_size = gltfComponentSize(acc.componentType);
	const size_t ec = static_cast<size_t>(expected_components);

	std::vector<float> result(acc.count * ec, 0.0f);

	if (acc.bufferView >= 0) {
		const auto& bv = gltf.bufferViews[static_cast<size_t>(acc.bufferView)];
		const auto& buf = gltf.buffers[static_cast<size_t>(bv.buffer)];
		int stride_val = acc.ByteStride(bv);
		size_t stride = stride_val > 0 ? static_cast<size_t>(stride_val) : comp_size * ec;
		for (size_t i = 0; i < acc.count; i++) {
			const uint8_t* base = &buf.data[bv.byteOffset + acc.byteOffset + i * stride];
			for (size_t c = 0; c < ec; c++)
				result[i * ec + c] = readGltfComponent(base + c * comp_size, acc.componentType, acc.normalized);
		}
	}

	if (acc.sparse.isSparse) {
		const auto& sidx = acc.sparse.indices;
		const auto& sval = acc.sparse.values;
		const auto& idx_bv = gltf.bufferViews[static_cast<size_t>(sidx.bufferView)];
		const auto& idx_buf = gltf.buffers[static_cast<size_t>(idx_bv.buffer)];
		const auto& val_bv = gltf.bufferViews[static_cast<size_t>(sval.bufferView)];
		const auto& val_buf = gltf.buffers[static_cast<size_t>(val_bv.buffer)];
		const size_t idx_comp_size = gltfComponentSize(sidx.componentType);
		const size_t sparse_count = static_cast<size_t>(acc.sparse.count);
		// Reject a sparse block that points past its buffer views (malformed/corrupt glTF).
		if (idx_bv.byteOffset + sidx.byteOffset + sparse_count * idx_comp_size > idx_buf.data.size()
		 || val_bv.byteOffset + sval.byteOffset + sparse_count * ec * comp_size > val_buf.data.size()) {
			VE_LOGW("glTF sparse accessor extends past its buffer view; skipping sparse overlay");
			return result;
		}
		const uint8_t* idx_base = &idx_buf.data[idx_bv.byteOffset + sidx.byteOffset];
		const uint8_t* val_base = &val_buf.data[val_bv.byteOffset + sval.byteOffset];
		for (size_t k = 0; k < sparse_count; k++) {
			const uint8_t* ip = idx_base + k * idx_comp_size;
			uint32_t target_index = 0;
			switch (sidx.componentType) {
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: target_index = *ip; break;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: target_index = *reinterpret_cast<const uint16_t*>(ip); break;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: target_index = *reinterpret_cast<const uint32_t*>(ip); break;
				default: target_index = *reinterpret_cast<const uint16_t*>(ip); break;
			}
			if (target_index >= acc.count)
				continue;
			for (size_t c = 0; c < ec; c++) {
				const uint8_t* vp = val_base + (k * ec + c) * comp_size;
				result[static_cast<size_t>(target_index) * ec + c] = readGltfComponent(vp, acc.componentType, acc.normalized);
			}
		}
	}

	return result;
}

static glm::quat convertQuatYupToZup(const glm::quat& q_gltf) {
	const glm::mat4 M_engine = YUP_TO_ZUP * glm::mat4_cast(q_gltf) * ZUP_TO_YUP;
	return glm::quat_cast(M_engine);
}

static glm::vec3 convertTranslationYupToZup(float x, float y, float z) {
	return {x, -z, y};
}

static glm::vec3 convertScaleYupToZup(float x, float y, float z) {
	return {x, z, y};
}

static int gltfTypeComponentCount(int type) {
	switch (type) {
		case TINYGLTF_TYPE_SCALAR: return 1;
		case TINYGLTF_TYPE_VEC2: return 2;
		case TINYGLTF_TYPE_VEC3: return 3;
		case TINYGLTF_TYPE_VEC4: return 4;
		default: return 1;
	}
}

static std::vector<VeAnimationClip> parseAnimations(
	const tinygltf::Model& gltf,
	const std::unordered_map<int, uint32_t>& gltf_to_loaded_idx,
	LoadProgress& progress) {

	std::vector<VeAnimationClip> raw_clips(gltf.animations.size());
	::ve::parallel::forIndexed(gltf.animations.size(), progress.cancelled,
		[&](size_t anim_idx) {
			const auto& anim = gltf.animations[anim_idx];
			VeAnimationClip clip;
		clip.name = anim.name;
		clip.duration = 0.0f;

		// Parse samplers
		clip.samplers.reserve(anim.samplers.size());
		for (const auto& gs : anim.samplers) {
			AnimationSampler sampler;
			sampler.timestamps = readAccessorFloats(gltf, gs.input, 1);
			int output_components = gltfTypeComponentCount(gltf.accessors[static_cast<size_t>(gs.output)].type);
			sampler.values = readAccessorFloats(gltf, gs.output, output_components);
			sampler.component_count = static_cast<uint8_t>(output_components);

			if (!sampler.timestamps.empty())
				clip.duration = std::max(clip.duration, sampler.timestamps.back());

			clip.samplers.push_back(std::move(sampler));
		}

		// Build unique target node mapping and parse channels
		std::unordered_map<int, uint32_t> gltf_node_to_clip_target;

		for (const auto& gc : anim.channels) {
			int gltf_node = gc.target_node;
			if (gltf_node < 0)
				continue;
			auto loaded_it = gltf_to_loaded_idx.find(gltf_node);
			if (loaded_it == gltf_to_loaded_idx.end())
				continue;

			AnimationChannel channel;

			// Map path string
			if (gc.target_path == "translation")
				channel.path = AnimationPath::Translation;
			else if (gc.target_path == "rotation")
				channel.path = AnimationPath::Rotation;
			else if (gc.target_path == "scale")
				channel.path = AnimationPath::Scale;
			else if (gc.target_path == "weights")
				channel.path = AnimationPath::Weights;
			else
				continue;

			// Map interpolation
			const auto& gs = anim.samplers[static_cast<size_t>(gc.sampler)];
			if (gs.interpolation == "STEP")
				channel.interpolation = AnimationInterpolation::Step;
			else if (gs.interpolation == "CUBICSPLINE")
				channel.interpolation = AnimationInterpolation::CubicSpline;
			else
				channel.interpolation = AnimationInterpolation::Linear;

			channel.sampler_index = static_cast<uint32_t>(gc.sampler);

			// Map glTF node to clip target index
			auto tit = gltf_node_to_clip_target.find(gltf_node);
			if (tit == gltf_node_to_clip_target.end()) {
				uint32_t target_idx = static_cast<uint32_t>(clip.target_node_indices.size());
				clip.target_node_indices.push_back(loaded_it->second);
				gltf_node_to_clip_target[gltf_node] = target_idx;
				channel.target_slot = target_idx;
			} else {
				channel.target_slot = tit->second;
			}

			clip.channels.push_back(channel);
		}

		// Apply coordinate conversion to sampler values based on channel paths
		// Track which samplers have been converted to avoid double-converting shared samplers
		std::vector<bool> sampler_converted(clip.samplers.size(), false);
		for (const auto& channel : clip.channels) {
			if (sampler_converted[channel.sampler_index])
				continue;
			sampler_converted[channel.sampler_index] = true;

			auto& sampler = clip.samplers[channel.sampler_index];
			bool is_cubicspline = channel.interpolation == AnimationInterpolation::CubicSpline;
			size_t n_keyframes = sampler.timestamps.size();

			if (channel.path == AnimationPath::Translation) {
				size_t values_per_kf = is_cubicspline ? 9 : 3; // cubicspline: 3 vec3 (in, val, out)
				for (size_t k = 0; k < n_keyframes; k++) {
					if (is_cubicspline) {
						for (int part = 0; part < 3; part++) {
							size_t base = k * values_per_kf + static_cast<size_t>(part) * 3;
							glm::vec3 v = convertTranslationYupToZup(sampler.values[base], sampler.values[base + 1], sampler.values[base + 2]);
							sampler.values[base] = v.x;
							sampler.values[base + 1] = v.y;
							sampler.values[base + 2] = v.z;
						}
					} else {
						size_t base = k * 3;
						glm::vec3 v = convertTranslationYupToZup(sampler.values[base], sampler.values[base + 1], sampler.values[base + 2]);
						sampler.values[base] = v.x;
						sampler.values[base + 1] = v.y;
						sampler.values[base + 2] = v.z;
					}
				}
			} else if (channel.path == AnimationPath::Rotation) {
				size_t values_per_kf = is_cubicspline ? 12 : 4; // cubicspline: 3 vec4
				for (size_t k = 0; k < n_keyframes; k++) {
					if (is_cubicspline) {
						for (int part = 0; part < 3; part++) {
							size_t base = k * values_per_kf + static_cast<size_t>(part) * 4;
							// glTF stores quaternion as [x, y, z, w]
							glm::quat q_gltf(sampler.values[base + 3], sampler.values[base], sampler.values[base + 1], sampler.values[base + 2]);
							glm::quat q_engine = convertQuatYupToZup(q_gltf);
							sampler.values[base] = q_engine.x;
							sampler.values[base + 1] = q_engine.y;
							sampler.values[base + 2] = q_engine.z;
							sampler.values[base + 3] = q_engine.w;
						}
					} else {
						size_t base = k * 4;
						glm::quat q_gltf(sampler.values[base + 3], sampler.values[base], sampler.values[base + 1], sampler.values[base + 2]);
						glm::quat q_engine = convertQuatYupToZup(q_gltf);
						sampler.values[base] = q_engine.x;
						sampler.values[base + 1] = q_engine.y;
						sampler.values[base + 2] = q_engine.z;
						sampler.values[base + 3] = q_engine.w;
					}
				}
			} else if (channel.path == AnimationPath::Scale) {
				size_t values_per_kf = is_cubicspline ? 9 : 3;
				for (size_t k = 0; k < n_keyframes; k++) {
					if (is_cubicspline) {
						for (int part = 0; part < 3; part++) {
							size_t base = k * values_per_kf + static_cast<size_t>(part) * 3;
							glm::vec3 v = convertScaleYupToZup(sampler.values[base], sampler.values[base + 1], sampler.values[base + 2]);
							sampler.values[base] = v.x;
							sampler.values[base + 1] = v.y;
							sampler.values[base + 2] = v.z;
						}
					} else {
						size_t base = k * 3;
						glm::vec3 v = convertScaleYupToZup(sampler.values[base], sampler.values[base + 1], sampler.values[base + 2]);
						sampler.values[base] = v.x;
						sampler.values[base + 1] = v.y;
						sampler.values[base + 2] = v.z;
					}
				}
			} else if (channel.path == AnimationPath::Weights) {
				if (n_keyframes > 0) {
					size_t per_kf = sampler.values.size() / n_keyframes; // target_count, or 3*target_count for cubicspline
					sampler.weights_target_count = static_cast<uint16_t>(is_cubicspline ? per_kf / 3 : per_kf);
				}
			}
		}

		if (!clip.channels.empty()) {
			VE_LOGI("Parsed animation '" << clip.name << "': " << clip.channels.size()
			        << " channels, " << clip.samplers.size() << " samplers, duration "
			        << clip.duration << "s");
			raw_clips[anim_idx] = std::move(clip);
		}
	});

	std::vector<VeAnimationClip> clips;
	clips.reserve(raw_clips.size());
	for (auto& c : raw_clips) {
		if (!c.channels.empty())
			clips.push_back(std::move(c));
	}
	return clips;
}

// Parse glTF skins into ModelSkin array. IBMs are converted from Y-up to Z-up
static std::vector<ModelSkin> parseSkins(const tinygltf::Model& gltf) {
	std::vector<ModelSkin> skins;
	skins.reserve(gltf.skins.size());
	for (size_t s = 0; s < gltf.skins.size(); s++) {
		const auto& gs = gltf.skins[s];
		ModelSkin skin;
		skin.joint_node_indices.assign(gs.joints.begin(), gs.joints.end());
		skin.skeleton_root_node = gs.skeleton;

		size_t joint_count = skin.joint_node_indices.size();
		if (gs.inverseBindMatrices >= 0) {
			std::vector<float> raw = readAccessorFloats(gltf, gs.inverseBindMatrices, 16);
			size_t mat_count = raw.size() / 16;
			skin.inverse_bind_matrices.reserve(mat_count);
			for (size_t j = 0; j < mat_count; j++) {
				glm::mat4 ibm_gltf(1.0f);
				for (int col = 0; col < 4; col++)
					for (int row = 0; row < 4; row++)
						ibm_gltf[col][row] = raw[j * 16 + static_cast<size_t>(col * 4 + row)];
				skin.inverse_bind_matrices.push_back(YUP_TO_ZUP * ibm_gltf * ZUP_TO_YUP);
			}
		}
		if (skin.inverse_bind_matrices.size() < joint_count)
			skin.inverse_bind_matrices.resize(joint_count, glm::mat4(1.0f));

		std::ostringstream det_log;
		for (size_t j = 0; j < skin.inverse_bind_matrices.size(); j++)
			det_log << ", IBM[" << j << "] det=" << glm::determinant(skin.inverse_bind_matrices[j]);
		VE_LOGD("[Skin " << s << "] " << joint_count << " joints" << det_log.str());

		skins.push_back(std::move(skin));
	}
	return skins;
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

// Extract cameras from glTF nodes. Each node with node.camera >= 0 produces an
// ExtractedCamera bound to that node, applied by addToScene as a CameraComponent.
static std::vector<ExtractedCamera> extractCameras(const tinygltf::Model& gltf) {
	std::vector<ExtractedCamera> out;
	for (size_t node_idx = 0; node_idx < gltf.nodes.size(); node_idx++) {
		const auto& node = gltf.nodes[node_idx];
		if (node.camera < 0 || node.camera >= static_cast<int>(gltf.cameras.size()))
			continue;
		const auto& src = gltf.cameras[static_cast<size_t>(node.camera)];
		ExtractedCamera cam;
		cam.node_idx = static_cast<int>(node_idx);
		cam.name = src.name.empty() ? node.name : src.name;
		if (src.type == "orthographic") {
			cam.perspective = false;
			cam.ortho_size = static_cast<float>(src.orthographic.ymag);
			cam.znear = static_cast<float>(src.orthographic.znear);
			cam.zfar  = static_cast<float>(src.orthographic.zfar);
		} else {
			cam.perspective = true;
			cam.yfov_radians = static_cast<float>(src.perspective.yfov);
			cam.znear = static_cast<float>(src.perspective.znear);
			// zfar is optional in glTF
			cam.zfar = src.perspective.zfar > 0.0 ? static_cast<float>(src.perspective.zfar) : 1000.0f;
		}
		VE_LOGI("Extracted camera " << cam.name << " from node " << node.name);
		out.push_back(std::move(cam));
	}
	return out;
}

// Extract KHR_lights_punctual lights from glTF extension data
static std::vector<ExtractedLight> extractPunctualLights(
    const tinygltf::Model& gltf,
    const std::vector<glm::mat4>& node_world_engine) {

	std::vector<ExtractedLight> punctual_lights;
	auto it_pl = gltf.extensions.find("KHR_lights_punctual");
	if (it_pl == gltf.extensions.end() || !it_pl->second.Has("lights") || !it_pl->second.Get("lights").IsArray())
		return punctual_lights;

	const auto& arr = it_pl->second.Get("lights").Get<tinygltf::Value::Array>();
	for (size_t i = 0; i < arr.size(); i++) {
		const auto& v = arr[i];
		if (!v.IsObject())
			continue;
		ExtractedLight light;
		if (v.Has("type") && v.Get("type").IsString()) {
			const std::string& type_str = v.Get("type").Get<std::string>();
			if (type_str == "directional")
				light.type = ExtractedLightType::Directional;
			else if (type_str == "spot")
				light.type = ExtractedLightType::Spot;
			else
				light.type = ExtractedLightType::Point;
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
		// Spot light cone angles from KHR_lights_punctual
		if (light.type == ExtractedLightType::Spot && v.Has("spot") && v.Get("spot").IsObject()) {
			const auto& spot = v.Get("spot");
			if (spot.Has("innerConeAngle") && spot.Get("innerConeAngle").IsNumber())
				light.inner_cone_angle = static_cast<float>(spot.Get("innerConeAngle").Get<double>());
			if (spot.Has("outerConeAngle") && spot.Get("outerConeAngle").IsNumber())
				light.outer_cone_angle = static_cast<float>(spot.Get("outerConeAngle").Get<double>());
		}
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
		pl.node_idx = static_cast<int>(node_idx);
		if (pl.name.empty() && !node.name.empty())
			pl.name = node.name;
		VE_LOGI("Extracted light " << pl.name << " from node " << node.name);
	}
	return punctual_lights;
}

// Extract lights from emissive materials, with vertex clustering for large meshes
static std::vector<ExtractedLight> extractEmissiveLights(
    const tinygltf::Model& gltf,
    const std::vector<glm::mat4>& node_world_engine,
    const std::vector<NodePrim>& node_primitives,
    const GeometryCenterExtent& geometry_center_extent,
    const std::vector<MaterialFactors>& material_factors,
    PosDedup& dedup) {

	std::vector<ExtractedLight> emissive_lights;
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

			auto pushLight = [&](const glm::vec3& world_pos, const std::string& suffix = "") -> bool {
				if (!dedup.insert(quantize(world_pos)).second)
					return false;
				emissive_lights.push_back({
					.type = ExtractedLightType::Point,
					.position = world_pos,
					.direction = glm::vec3(0.0f, 0.0f, -1.0f),
					.color = color_n,
					.intensity = intensity,
					.range = std::max(diag * 1.25f, 0.25f),
					.name = mat_name + ": " + individual_name + suffix,
					.node_idx = np.node_idx
				});
				emissive_light_count++;
				return true;
			};

			if (diag < EMISSIVE_CLUSTER_EXTENT) {
				const glm::vec3& center = ce_it->second.first;
				glm::vec3 world_pos = glm::vec3(W * glm::vec4(center, 1.f));
				if (!pushLight(world_pos))
					continue;
			} else {
				const tinygltf::Accessor* pos_acc = findPositionAccessor(np);
				if (!pos_acc) {
					const glm::vec3& center = ce_it->second.first;
					glm::vec3 world_pos = glm::vec3(W * glm::vec4(center, 1.f));
					if (!pushLight(world_pos))
						continue;
					continue;
				}
				const auto& bv = gltf.bufferViews[static_cast<size_t>(pos_acc->bufferView)];
				const auto& buf = gltf.buffers[static_cast<size_t>(bv.buffer)];
				const uint8_t* data = buf.data.data() + bv.byteOffset + pos_acc->byteOffset;
				int sv = pos_acc->ByteStride(bv);
				size_t comp_sz = gltfComponentSize(pos_acc->componentType);
				size_t stride = sv > 0 ? static_cast<size_t>(sv) : comp_sz * 3;

				std::vector<glm::vec3> positions(pos_acc->count);
				for (size_t vi = 0; vi < pos_acc->count; vi++) {
					const uint8_t* vp = data + vi * stride;
					float x = readGltfComponent(vp + 0 * comp_sz, pos_acc->componentType, pos_acc->normalized);
					float y = readGltfComponent(vp + 1 * comp_sz, pos_acc->componentType, pos_acc->normalized);
					float z = readGltfComponent(vp + 2 * comp_sz, pos_acc->componentType, pos_acc->normalized);
					positions[vi] = {x, -z, y};  // Y-up to Z-up
				}

				std::vector<glm::vec3> centroids = clusterVertices(positions, EMISSIVE_CLUSTER_GAP, EMISSIVE_CLUSTER_EXTENT);

				for (size_t ci = 0; ci < centroids.size(); ci++) {
					glm::vec3 world_pos = glm::vec3(W * glm::vec4(centroids[ci], 1.f));
					pushLight(world_pos, " [" + std::to_string(ci) + "]");
				}
			}
			if (emissive_light_count >= ve::MAX_CLUSTER_LIGHTS - 1)
				VE_LOGW("Reached maximum light count while extracting emissive lights from model; some lights may be missing");
		}
	}
	return emissive_lights;
}

// Geometry key for mesh deduplication: same geometry+material shares one VeMesh.
static std::string geometryKey(const tinygltf::Primitive& primitive, size_t material_index) {
	std::string key = "mat_" + std::to_string(material_index) + "_idx_" + std::to_string(primitive.indices);
	for (const auto& [attr_name, accessor_idx] : primitive.attributes)
		key += "_" + attr_name + "_" + std::to_string(accessor_idx);
	return key;
}

// Process a single glTF primitive into a ProcessedMesh (CPU only, no Vulkan calls).
// Extracts vertices/indices, generates MikkTSpace tangents, deduplicates via meshoptimizer,
// builds LOD levels and meshlet data. If out_center_extent is non-null, writes (center, diagonal).
static ProcessedMesh processPrimitive(
	const tinygltf::Primitive& primitive, const tinygltf::Model& m,
	const std::string& mesh_id,
	std::pair<glm::vec3, float>* out_center_extent = nullptr) {

	std::vector<VeMesh::Vertex> vertices;
	std::vector<uint32_t> indices;

	const tinygltf::Accessor& pos_accessor = m.accessors[static_cast<size_t>(primitive.attributes.at("POSITION"))];
	const tinygltf::BufferView& pos_bv = m.bufferViews[static_cast<size_t>(pos_accessor.bufferView)];
	const tinygltf::Buffer& pos_buf = m.buffers[static_cast<size_t>(pos_bv.buffer)];

	const tinygltf::Accessor* index_accessor = (primitive.indices >= 0) ? &m.accessors[static_cast<size_t>(primitive.indices)] : nullptr;
	const tinygltf::BufferView* index_bv = index_accessor ? &m.bufferViews[static_cast<size_t>(index_accessor->bufferView)] : nullptr;
	const tinygltf::Buffer* index_buf = index_bv ? &m.buffers[static_cast<size_t>(index_bv->buffer)] : nullptr;

	bool has_normals = primitive.attributes.find("NORMAL") != primitive.attributes.end();
	const tinygltf::Accessor* normal_acc = has_normals ? &m.accessors[static_cast<size_t>(primitive.attributes.at("NORMAL"))] : nullptr;
	const tinygltf::BufferView* normal_bv = has_normals ? &m.bufferViews[static_cast<size_t>(normal_acc->bufferView)] : nullptr;
	const tinygltf::Buffer* normal_buf = has_normals ? &m.buffers[static_cast<size_t>(normal_bv->buffer)] : nullptr;

	bool has_tangents = primitive.attributes.find("TANGENT") != primitive.attributes.end();
	const tinygltf::Accessor* tangent_acc = has_tangents ? &m.accessors[static_cast<size_t>(primitive.attributes.at("TANGENT"))] : nullptr;
	const tinygltf::BufferView* tangent_bv = has_tangents ? &m.bufferViews[static_cast<size_t>(tangent_acc->bufferView)] : nullptr;
	const tinygltf::Buffer* tangent_buf = has_tangents ? &m.buffers[static_cast<size_t>(tangent_bv->buffer)] : nullptr;

	bool has_tex_coords = primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end();
	const tinygltf::Accessor* tex_acc = has_tex_coords ? &m.accessors[static_cast<size_t>(primitive.attributes.at("TEXCOORD_0"))] : nullptr;
	const tinygltf::BufferView* tex_bv = has_tex_coords ? &m.bufferViews[static_cast<size_t>(tex_acc->bufferView)] : nullptr;
	const tinygltf::Buffer* tex_buf = has_tex_coords ? &m.buffers[static_cast<size_t>(tex_bv->buffer)] : nullptr;

	// COLOR_0 is optional and may be VEC3 or VEC4, float or normalized u8/u16: normalize to linear vec4
	bool has_colors = primitive.attributes.find("COLOR_0") != primitive.attributes.end();
	const tinygltf::Accessor* color_acc = has_colors ? &m.accessors[static_cast<size_t>(primitive.attributes.at("COLOR_0"))] : nullptr;
	const tinygltf::BufferView* color_bv = has_colors ? &m.bufferViews[static_cast<size_t>(color_acc->bufferView)] : nullptr;
	const tinygltf::Buffer* color_buf = has_colors ? &m.buffers[static_cast<size_t>(color_bv->buffer)] : nullptr;
	const int color_components = has_colors ? (color_acc->type == TINYGLTF_TYPE_VEC4 ? 4 : 3) : 0;

	int pos_stride_val = pos_accessor.ByteStride(pos_bv);
	const size_t pos_stride = pos_stride_val > 0 ? static_cast<size_t>(pos_stride_val) : gltfComponentSize(pos_accessor.componentType) * 3;
	int normal_stride_val = has_normals ? normal_acc->ByteStride(*normal_bv) : 0;
	const size_t normal_stride = normal_stride_val > 0 ? static_cast<size_t>(normal_stride_val) : (has_normals ? gltfComponentSize(normal_acc->componentType) * 3 : size_t{12});
	size_t tex_stride = 8;
	if (has_tex_coords) {
		int ts = tex_acc->ByteStride(*tex_bv);
		if (ts > 0) tex_stride = static_cast<size_t>(ts);
		else tex_stride = gltfComponentSize(tex_acc->componentType) * 2;
	}
	int tangent_stride_val = has_tangents ? tangent_acc->ByteStride(*tangent_bv) : 0;
	const size_t tangent_stride = tangent_stride_val > 0 ? static_cast<size_t>(tangent_stride_val) : (has_tangents ? gltfComponentSize(tangent_acc->componentType) * 4 : size_t{16});
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

	int color_stride_val = has_colors ? color_acc->ByteStride(*color_bv) : 0;
	const size_t color_stride = color_stride_val > 0 ? static_cast<size_t>(color_stride_val) : (has_colors ? gltfComponentSize(color_acc->componentType) * static_cast<size_t>(color_components) : size_t{0});

	const size_t pos_comp_size = gltfComponentSize(pos_accessor.componentType);
	const size_t normal_comp_size = has_normals ? gltfComponentSize(normal_acc->componentType) : 4;
	const size_t tangent_comp_size = has_tangents ? gltfComponentSize(tangent_acc->componentType) : 4;
	const size_t tex_comp_size = has_tex_coords ? gltfComponentSize(tex_acc->componentType) : 4;
	const size_t color_comp_size = has_colors ? gltfComponentSize(color_acc->componentType) : 4;

	// Stage 1: build vertex array and indices (same as createPrimitiveMesh)
	vertices.reserve(static_cast<size_t>(pos_accessor.count));
	for (size_t i = 0; i < pos_accessor.count; i++) {
		VeMesh::Vertex vertex{};
		const uint8_t* pos_base = &pos_buf.data[pos_bv.byteOffset + pos_accessor.byteOffset + i * pos_stride];
		float px = readGltfComponent(pos_base + 0 * pos_comp_size, pos_accessor.componentType, pos_accessor.normalized);
		float py = readGltfComponent(pos_base + 1 * pos_comp_size, pos_accessor.componentType, pos_accessor.normalized);
		float pz = readGltfComponent(pos_base + 2 * pos_comp_size, pos_accessor.componentType, pos_accessor.normalized);
		vertex.pos = {px, -pz, py};

		if (has_normals) {
			const uint8_t* normal_base = &normal_buf->data[normal_bv->byteOffset + normal_acc->byteOffset + i * normal_stride];
			float nx = readGltfComponent(normal_base + 0 * normal_comp_size, normal_acc->componentType, normal_acc->normalized);
			float ny = readGltfComponent(normal_base + 1 * normal_comp_size, normal_acc->componentType, normal_acc->normalized);
			float nz = readGltfComponent(normal_base + 2 * normal_comp_size, normal_acc->componentType, normal_acc->normalized);
			vertex.normal = {nx, -nz, ny};
			if (normal_acc->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
				float len = glm::length(vertex.normal);
				if (len > 1e-6f)
					vertex.normal /= len;
			}
		} else {
			vertex.normal = {0, 0, 0};
		}

		if (has_tex_coords && tex_stride > 0) {
			const uint8_t* tex_base = &tex_buf->data[tex_bv->byteOffset + tex_acc->byteOffset + i * tex_stride];
			float tu = readGltfComponent(tex_base + 0 * tex_comp_size, tex_acc->componentType, tex_acc->normalized);
			float tv = readGltfComponent(tex_base + 1 * tex_comp_size, tex_acc->componentType, tex_acc->normalized);
			vertex.tex_coord = {tu, tv};
		} else {
			vertex.tex_coord = {0, 0};
		}

		if (has_tangents && tangent_stride > 0) {
			const uint8_t* t_base = &tangent_buf->data[tangent_bv->byteOffset + tangent_acc->byteOffset + i * tangent_stride];
			float tx = readGltfComponent(t_base + 0 * tangent_comp_size, tangent_acc->componentType, tangent_acc->normalized);
			float ty = readGltfComponent(t_base + 1 * tangent_comp_size, tangent_acc->componentType, tangent_acc->normalized);
			float tz = readGltfComponent(t_base + 2 * tangent_comp_size, tangent_acc->componentType, tangent_acc->normalized);
			float tw = readGltfComponent(t_base + 3 * tangent_comp_size, tangent_acc->componentType, tangent_acc->normalized);
			glm::vec3 T(tx, -tz, ty);
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
			float w = (tw >= 0.0f) ? 1.0f : -1.0f;
			vertex.tangent = glm::vec4(T, w);
		} else {
			vertex.tangent = {0, 0, 0, 0};
		}

		if (has_colors && color_stride > 0) {
			const uint8_t* c_base = &color_buf->data[color_bv->byteOffset + color_acc->byteOffset + i * color_stride];
			float cr = readGltfComponent(c_base + 0 * color_comp_size, color_acc->componentType, color_acc->normalized);
			float cg = readGltfComponent(c_base + 1 * color_comp_size, color_acc->componentType, color_acc->normalized);
			float cb = readGltfComponent(c_base + 2 * color_comp_size, color_acc->componentType, color_acc->normalized);
			float ca = (color_components == 4) ? readGltfComponent(c_base + 3 * color_comp_size, color_acc->componentType, color_acc->normalized) : 1.0f;
			vertex.color = {cr, cg, cb, ca};
		}
		vertices.push_back(vertex);
	}

	std::vector<VeMesh::SkinVertex> skin_vertices;
	auto joints_it = primitive.attributes.find("JOINTS_0");
	auto weights_it = primitive.attributes.find("WEIGHTS_0");
	if (joints_it != primitive.attributes.end() && weights_it != primitive.attributes.end()) {
		const tinygltf::Accessor& joints_acc = m.accessors[static_cast<size_t>(joints_it->second)];
		const tinygltf::BufferView& joints_bv = m.bufferViews[static_cast<size_t>(joints_acc.bufferView)];
		const tinygltf::Buffer& joints_buf = m.buffers[static_cast<size_t>(joints_bv.buffer)];
		const size_t joints_comp_size = gltfComponentSize(joints_acc.componentType);
		int joints_stride_val = joints_acc.ByteStride(joints_bv);
		const size_t joints_stride = joints_stride_val > 0 ? static_cast<size_t>(joints_stride_val) : joints_comp_size * 4;

		const tinygltf::Accessor& weights_acc = m.accessors[static_cast<size_t>(weights_it->second)];
		const tinygltf::BufferView& weights_bv = m.bufferViews[static_cast<size_t>(weights_acc.bufferView)];
		const tinygltf::Buffer& weights_buf = m.buffers[static_cast<size_t>(weights_bv.buffer)];
		const size_t weights_comp_size = gltfComponentSize(weights_acc.componentType);
		int weights_stride_val = weights_acc.ByteStride(weights_bv);
		const size_t weights_stride = weights_stride_val > 0 ? static_cast<size_t>(weights_stride_val) : weights_comp_size * 4;

		const size_t source_count = std::min({static_cast<size_t>(joints_acc.count),
		                                      static_cast<size_t>(weights_acc.count),
		                                      vertices.size()});
		skin_vertices.resize(vertices.size(), VeMesh::SkinVertex{{0,0,0,0},{65535,0,0,0}});

		for (size_t i = 0; i < source_count; i++) {
			const uint8_t* j_base = &joints_buf.data[joints_bv.byteOffset + joints_acc.byteOffset + i * joints_stride];
			const uint8_t* w_base = &weights_buf.data[weights_bv.byteOffset + weights_acc.byteOffset + i * weights_stride];

			VeMesh::SkinVertex sv{};
			for (int c = 0; c < 4; c++) {
				const uint8_t* jp = j_base + static_cast<size_t>(c) * joints_comp_size;
				sv.joints[c] = (joints_acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
				             ? *jp : *reinterpret_cast<const uint16_t*>(jp);
				float w = readGltfComponent(w_base + static_cast<size_t>(c) * weights_comp_size,
				                            weights_acc.componentType, weights_acc.normalized);
				float wn = std::clamp(w, 0.0f, 1.0f);
				sv.weights[c] = static_cast<uint16_t>(wn * 65535.0f + 0.5f);
			}
			skin_vertices[i] = sv;
		}
	}

	// Morph targets
	const size_t morph_target_count = primitive.targets.size();
	std::vector<glm::vec3> morph_pos_deltas;
	std::vector<glm::vec3> morph_normal_deltas;
	if (morph_target_count > 0) {
		morph_pos_deltas.assign(morph_target_count * vertices.size(), glm::vec3(0.0f));
		morph_normal_deltas.assign(morph_target_count * vertices.size(), glm::vec3(0.0f));
		for (size_t t = 0; t < morph_target_count; t++) {
			const auto& target = primitive.targets[t];
			auto pit = target.find("POSITION");
			if (pit != target.end()) {
				std::vector<float> pd = readAccessorFloats(m, pit->second, 3);
				size_t n = std::min(vertices.size(), pd.size() / 3);
				for (size_t i = 0; i < n; i++)
					morph_pos_deltas[t * vertices.size() + i] = {pd[i * 3 + 0], -pd[i * 3 + 2], pd[i * 3 + 1]};
			}
			auto nit = target.find("NORMAL");
			if (nit != target.end()) {
				std::vector<float> nd = readAccessorFloats(m, nit->second, 3);
				size_t n = std::min(vertices.size(), nd.size() / 3);
				for (size_t i = 0; i < n; i++)
					morph_normal_deltas[t * vertices.size() + i] = {nd[i * 3 + 0], -nd[i * 3 + 2], nd[i * 3 + 1]};
			}
		}
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

	if (!has_normals && indices.size() >= 3 && (indices.size() % 3) == 0) {
		for (auto& v : vertices)
			v.normal = glm::vec3(0.0f);
		for (size_t i = 0; i + 2 < indices.size(); i += 3) {
			const glm::vec3& p0 = vertices[indices[i + 0]].pos;
			const glm::vec3& p1 = vertices[indices[i + 1]].pos;
			const glm::vec3& p2 = vertices[indices[i + 2]].pos;
			glm::vec3 face_n = glm::cross(p1 - p0, p2 - p0);
			vertices[indices[i + 0]].normal += face_n;
			vertices[indices[i + 1]].normal += face_n;
			vertices[indices[i + 2]].normal += face_n;
		}
		for (auto& v : vertices) {
			float len = glm::length(v.normal);
			if (len > 1e-6f)
				v.normal /= len;
			else
				v.normal = glm::vec3(0, 0, 1);
		}
	}

	if (!has_tangents && !has_tex_coords) {
		for (auto& v : vertices) {
			glm::vec3 n = v.normal;
			glm::vec3 ref = std::abs(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
			glm::vec3 t = glm::normalize(glm::cross(ref, n));
			v.tangent = glm::vec4(t, 1.0f);
		}
	}

	// Stage 2: MikkTSpace tangents
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
		if (genTangSpaceDefault(&ctx))
			VE_LOGI("MikkTSpace tangent generation successful for mesh " << mesh_id);
		else
			VE_LOGW("MikkTSpace tangent generation failed for mesh " << mesh_id);
	}

	// Stage 3: deduplicate
	std::vector<unsigned int> remap(vertices.size());
	size_t unique_count = meshopt_generateVertexRemap(
		remap.data(), indices.data(), indices.size(),
		vertices.data(), vertices.size(), sizeof(VeMesh::Vertex));
	std::vector<VeMesh::Vertex> out_vertices(unique_count);
	std::vector<uint32_t> out_indices(indices.size());
	meshopt_remapVertexBuffer(out_vertices.data(), vertices.data(), vertices.size(), sizeof(VeMesh::Vertex), remap.data());
	meshopt_remapIndexBuffer(out_indices.data(), indices.data(), indices.size(), remap.data());

	std::vector<VeMesh::SkinVertex> out_skin_vertices;
	if (!skin_vertices.empty()) {
		out_skin_vertices.resize(unique_count);
		meshopt_remapVertexBuffer(out_skin_vertices.data(), skin_vertices.data(),
		                          skin_vertices.size(), sizeof(VeMesh::SkinVertex), remap.data());
	}

	std::vector<glm::vec3> out_morph_pos_deltas;
	std::vector<glm::vec3> out_morph_normal_deltas;
	if (morph_target_count > 0) {
		out_morph_pos_deltas.resize(morph_target_count * unique_count);
		out_morph_normal_deltas.resize(morph_target_count * unique_count);
		for (size_t t = 0; t < morph_target_count; t++) {
			meshopt_remapVertexBuffer(out_morph_pos_deltas.data() + t * unique_count,
			                          morph_pos_deltas.data() + t * vertices.size(),
			                          vertices.size(), sizeof(glm::vec3), remap.data());
			meshopt_remapVertexBuffer(out_morph_normal_deltas.data() + t * unique_count,
			                          morph_normal_deltas.data() + t * vertices.size(),
			                          vertices.size(), sizeof(glm::vec3), remap.data());
		}
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

	// Per-joint mesh-local extents: the AABB of vertices weighted (>0) to each joint.
	// Used at runtime to compute a tight world AABB for skinned meshes
	std::vector<VeMesh::AABB> joint_mesh_local_extents;
	if (!out_skin_vertices.empty()) {
		uint16_t max_joint = 0;
		for (const auto& sv : out_skin_vertices)
			for (int c = 0; c < 4; c++)
				if (sv.weights[c] > 0)
					max_joint = std::max(max_joint, sv.joints[c]);
		joint_mesh_local_extents.resize(static_cast<size_t>(max_joint) + 1,
			VeMesh::AABB{glm::vec3(std::numeric_limits<float>::max()),
			             glm::vec3(std::numeric_limits<float>::lowest())});
		for (size_t v = 0; v < out_vertices.size(); v++) {
			const auto& sv = out_skin_vertices[v];
			const glm::vec3& pos = out_vertices[v].pos;
			for (int c = 0; c < 4; c++) {
				if (sv.weights[c] == 0)
					continue;
				auto& aabb = joint_mesh_local_extents[sv.joints[c]];
				aabb.min = glm::min(aabb.min, pos);
				aabb.max = glm::max(aabb.max, pos);
			}
		}
		// Joints with no weighted vertices produces a zero-volume box that contributes
		// nothing to the union.
		for (auto& aabb : joint_mesh_local_extents) {
			if (aabb.min.x > aabb.max.x)
				aabb = {glm::vec3(0.0f), glm::vec3(0.0f)};
		}
	}

	// Stage 4a: optimize
	meshopt_optimizeVertexCache(out_indices.data(), out_indices.data(),
	                            out_indices.size(), out_vertices.size());
	meshopt_optimizeOverdraw(out_indices.data(), out_indices.data(),
	                         out_indices.size(),
	                         &out_vertices[0].pos.x, out_vertices.size(),
	                         sizeof(VeMesh::Vertex), 1.05f);

	// Stage 4b: LOD generation
	std::vector<std::vector<uint32_t>> lod_indices;
	size_t base_index_count = out_indices.size();
	const size_t attr_count = 5;
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
		if (target_count >= out_indices.size())
			break;

		std::vector<uint32_t> simplified(out_indices.size());
		float result_error = 0.0f;
		size_t result_count = meshopt_simplifyWithAttributes(
			simplified.data(), out_indices.data(), out_indices.size(),
			&out_vertices[0].pos.x, out_vertices.size(), sizeof(VeMesh::Vertex),
			vertex_attributes.data(), sizeof(float) * attr_count,
			attribute_weights, attr_count, nullptr,
			target_count, ve::LOD_ERROR_THRESHOLD, meshopt_SimplifyLockBorder, &result_error);
		simplified.resize(result_count);

		size_t prev_count = lod_indices.empty() ? base_index_count : lod_indices.back().size();
		if (result_count >= prev_count * 95 / 100)
			break;

		meshopt_optimizeVertexCache(simplified.data(), simplified.data(),
		                            simplified.size(), out_vertices.size());
		lod_indices.push_back(std::move(simplified));
	}

	// Compute AABB
	ProcessedMesh result;
	result.resource_id = mesh_id;
	if (!out_vertices.empty()) {
		glm::vec3 mn(out_vertices[0].pos), mx(out_vertices[0].pos);
		for (const auto& vert : out_vertices) {
			mn = glm::min(mn, vert.pos);
			mx = glm::max(mx, vert.pos);
		}
		result.local_aabb = {mn, mx};
	}

	// Conservative AABB for morph targets
	if (morph_target_count > 0 && !out_vertices.empty()) {
		glm::vec3 mn(std::numeric_limits<float>::max());
		glm::vec3 mx(std::numeric_limits<float>::lowest());
		for (size_t v = 0; v < out_vertices.size(); v++) {
			glm::vec3 lo = out_vertices[v].pos;
			glm::vec3 hi = out_vertices[v].pos;
			for (size_t t = 0; t < morph_target_count; t++) {
				const glm::vec3& d = out_morph_pos_deltas[t * unique_count + v];
				lo += glm::min(d, glm::vec3(0.0f));
				hi += glm::max(d, glm::vec3(0.0f));
			}
			mn = glm::min(mn, lo);
			mx = glm::max(mx, hi);
		}
		result.morph_local_aabb = {mn, mx};
	}

	// Build meshlet data
	result.meshlet_data = VeMesh::buildMeshletData(out_vertices, out_indices, lod_indices);

	// Precompute position-only array for shadow rendering
	result.shadow_positions.reserve(out_vertices.size());
	for (const auto& v : out_vertices)
		result.shadow_positions.push_back(v.pos);

	result.vertices = std::move(out_vertices);
	result.skin_vertices = std::move(out_skin_vertices);
	result.joint_mesh_local_extents = std::move(joint_mesh_local_extents);
	result.morph_target_count = static_cast<uint32_t>(morph_target_count);
	result.morph_pos_deltas = std::move(out_morph_pos_deltas);
	result.morph_normal_deltas = std::move(out_morph_normal_deltas);
	result.indices = std::move(out_indices);
	result.lod_indices = std::move(lod_indices);
	return result;
}

// Unique primitive descriptor produced by the discovery pass.
struct UniquePrimitive {
	const tinygltf::Primitive* primitive;
	std::string key;
	std::string mesh_id;
};

struct PendingMeshAttach {
	uint32_t node_idx;
	size_t prim_idx;
	std::string key;
	size_t mat_idx;
	std::string parent_node_name;
	int skin_idx;
};

// Context for CPU-only node processing
struct GltfCpuLoadContext {
	const tinygltf::Model& gltf;
	const std::string& model_path_str;
	const std::vector<ProcessedMaterial>& materials;
	std::vector<NodePrim>& node_primitives;
	std::vector<UniquePrimitive>& unique_primitives;
	std::unordered_map<std::string, int>& key_to_unique_idx;
	std::vector<PendingMeshAttach>& pending_attachments;
	LoadProgress& progress;
};

// Walk the glTF node tree to build the node graph and collect unique primitives
static void discoverNodeCpu(
	int gltf_node_idx, int parent_node_idx,
	GltfCpuLoadContext& ctx,
	std::vector<ModelNode>& nodes,
	std::vector<std::pair<uint32_t, uint32_t>>& parent_links,
	std::unordered_set<uint32_t>& root_indices,
	std::unordered_map<int, uint32_t>& gltf_to_loaded_idx) {

	if (ctx.progress.cancelled.load())
		return;

	const auto& node = ctx.gltf.nodes[static_cast<size_t>(gltf_node_idx)];
	NodeTRS trs = getNodeTRS(node);

	std::vector<float> morph_weights;
	const std::vector<double>* src_weights = nullptr;
	if (!node.weights.empty())
		src_weights = &node.weights;
	else if (node.mesh >= 0 && !ctx.gltf.meshes[static_cast<size_t>(node.mesh)].weights.empty())
		src_weights = &ctx.gltf.meshes[static_cast<size_t>(node.mesh)].weights;
	if (src_weights) {
		morph_weights.reserve(src_weights->size());
		for (double w : *src_weights)
			morph_weights.push_back(static_cast<float>(w));
	}

	uint32_t node_idx = static_cast<uint32_t>(nodes.size());
	gltf_to_loaded_idx[gltf_node_idx] = node_idx;
	nodes.push_back({
		.name = node.name,
		.translation = trs.translation,
		.rotation = trs.rotation,
		.scale = trs.scale,
		.skin_idx = node.skin,
		.morph_weights = std::move(morph_weights)
	});

	if (parent_node_idx >= 0)
		parent_links.emplace_back(node_idx, static_cast<uint32_t>(parent_node_idx));
	else
		root_indices.insert(node_idx);

	if (node.mesh >= 0) {
		const auto& mesh = ctx.gltf.meshes[static_cast<size_t>(node.mesh)];
		for (size_t prim_idx = 0; prim_idx < mesh.primitives.size(); prim_idx++) {
			const auto& primitive = mesh.primitives[prim_idx];
			size_t mat_idx = (primitive.material >= 0 && static_cast<size_t>(primitive.material) < ctx.materials.size())
			                    ? static_cast<size_t>(primitive.material) : 0;
			std::string key = geometryKey(primitive, mat_idx);
			if (ctx.key_to_unique_idx.find(key) == ctx.key_to_unique_idx.end()) {
				int unique_idx = static_cast<int>(ctx.unique_primitives.size());
				ctx.unique_primitives.push_back({
					.primitive = &primitive,
					.key = key,
					.mesh_id = ctx.model_path_str + "::" + key,
				});
				ctx.key_to_unique_idx[key] = unique_idx;
			}
			ctx.node_primitives.push_back({gltf_node_idx, key, mat_idx});
			ctx.pending_attachments.push_back({
				.node_idx = node_idx,
				.prim_idx = prim_idx,
				.key = std::move(key),
				.mat_idx = mat_idx,
				.parent_node_name = node.name,
				.skin_idx = node.skin,
			});
		}
	}

	for (int child_idx : node.children)
		discoverNodeCpu(child_idx, static_cast<int>(node_idx), ctx,
		                nodes, parent_links, root_indices, gltf_to_loaded_idx);
}

// Lightweight image loader for async path
static bool LoadImageDataCpuOnly(tinygltf::Image* image, const int /*image_idx*/, std::string* /*err*/,
                                 std::string* /*warn*/, int /*req_width*/, int /*req_height*/,
                                 const unsigned char* bytes, int size, void* /*user_data*/) {
	if (!image->uri.empty()) {
		// External image: engine resolves via URI later
		image->image.clear();
		image->as_is = true;
		return true;
	}
	// Embedded image (glb): keep raw bytes for parallel decode
	image->width = image->height = image->component = -1;
	image->bits = image->pixel_type = -1;
	image->as_is = true;
	image->image.assign(bytes, bytes + size);
	return true;
}

// Every glTF extension this loader actually consumes. Anything in extensionsUsed
// outside this set is silently dropped at parse time, so warn the author about it.
static const std::unordered_set<std::string> s_supported_extensions = {
	"KHR_materials_pbrSpecularGlossiness",
	"KHR_materials_emissive_strength",
	"KHR_materials_transmission",
	"KHR_materials_ior",
	"KHR_materials_specular",
	"KHR_texture_basisu",
	"KHR_lights_punctual",
	"KHR_texture_transform",
	"EXT_meshopt_compression",
};

static void warnUnsupportedExtensions(const tinygltf::Model& gltf, const std::filesystem::path& model_path) {
	for (const auto& ext : gltf.extensionsUsed) {
		if (s_supported_extensions.count(ext))
			continue;
		bool required = std::find(gltf.extensionsRequired.begin(), gltf.extensionsRequired.end(), ext) != gltf.extensionsRequired.end();
		VE_LOGW("glTF '" << model_path.filename().string() << "' uses unsupported extension '"
			<< ext << "'" << (required ? " (required)" : "") << "; its data will be ignored and the model may render incorrectly");
	}
}

// Load a glTF/GLB file via tinygltf. Returns true on success.
// When cpu_only is true, uses the lightweight loader that skips external images.
static bool loadGltfFile(const std::filesystem::path& model_path, tinygltf::Model& gltf, bool cpu_only = false) {
	tinygltf::TinyGLTF loader;
	loader.SetImagesAsIs(true);
	tinygltf::LoadImageDataOption load_opt;
	load_opt.as_is = true;
	load_opt.preserve_channels = false;
	if (cpu_only)
		loader.SetImageLoader(LoadImageDataCpuOnly, nullptr);
	else
		loader.SetImageLoader(LoadImageDataVeModel, &load_opt);
	std::string err, warn;
	bool ret = false;
	std::string path_ext = model_path.extension().string();
	std::transform(path_ext.begin(), path_ext.end(), path_ext.begin(), ::tolower);
	if (path_ext == ".glb")
		ret = loader.LoadBinaryFromFile(&gltf, &err, &warn, model_path.string());
	else if (path_ext == ".gltf")
		ret = loader.LoadASCIIFromFile(&gltf, &err, &warn, model_path.string());
	else {
		VE_LOGE("Unsupported file extension for glTF model: " << model_path);
		return false;
	}
	if (!ret) {
		VE_LOGE("Failed to load glTF: " << err);
		return false;
	}
	if (!warn.empty())
		VE_LOGW("glTF warning: " << warn);
	warnUnsupportedExtensions(gltf, model_path);
	return true;
}

// Decompress buffer views that use EXT_meshopt_compression.
// Decoded data is stored in a new buffer appended to the model.
static void decompressMeshopt(tinygltf::Model& gltf) {
	struct Job {
		size_t bv_idx;
		const unsigned char* src;
		size_t src_length;
		size_t count;
		size_t stride;
		size_t decode_offset;
		size_t decoded_size;
		const std::string* mode;
		const std::string* filter;   // null if no filter
	};
	std::vector<Job> jobs;
	jobs.reserve(gltf.bufferViews.size() / 4);

	size_t total_size = 0;
	for (size_t i = 0; i < gltf.bufferViews.size(); i++) {
		auto& bv = gltf.bufferViews[i];
		auto ext_it = bv.extensions.find("EXT_meshopt_compression");
		if (ext_it == bv.extensions.end())
			continue;

		const auto& ext = ext_it->second;
		Job j;
		j.bv_idx = i;
		int src_buffer = ext.Get("buffer").GetNumberAsInt();
		size_t src_offset = static_cast<size_t>(ext.Get("byteOffset").GetNumberAsInt());
		j.src_length = static_cast<size_t>(ext.Get("byteLength").GetNumberAsInt());
		j.stride = static_cast<size_t>(ext.Get("byteStride").GetNumberAsInt());
		j.count = static_cast<size_t>(ext.Get("count").GetNumberAsInt());
		j.mode = &ext.Get("mode").Get<std::string>();
		j.src = gltf.buffers[static_cast<size_t>(src_buffer)].data.data() + src_offset;
		j.decoded_size = j.count * j.stride;
		j.decode_offset = total_size;
		j.filter = ext.Has("filter") ? &ext.Get("filter").Get<std::string>() : nullptr;
		total_size += j.decoded_size;
		jobs.push_back(j);
	}
	if (jobs.empty())
		return;

	int decode_buf_idx = static_cast<int>(gltf.buffers.size());
	gltf.buffers.emplace_back();
	auto& decode_buf = gltf.buffers[static_cast<size_t>(decode_buf_idx)];
	decode_buf.data.resize(total_size);

	std::vector<uint8_t> success(jobs.size(), 0);
	std::atomic<bool> cancel{false};
	::ve::parallel::forIndexed(jobs.size(), cancel, [&](size_t i) {
		const Job& j = jobs[i];
		unsigned char* dst = decode_buf.data.data() + j.decode_offset;
		int result = -1;
		if (*j.mode == "ATTRIBUTES")
			result = meshopt_decodeVertexBuffer(dst, j.count, j.stride, j.src, j.src_length);
		else if (*j.mode == "TRIANGLES")
			result = meshopt_decodeIndexBuffer(dst, j.count, j.stride, j.src, j.src_length);
		else if (*j.mode == "INDICES")
			result = meshopt_decodeIndexSequence(dst, j.count, j.stride, j.src, j.src_length);
		if (result != 0)
			return;
		if (j.filter) {
			if (*j.filter == "OCTAHEDRAL")
				meshopt_decodeFilterOct(dst, j.count, j.stride);
			else if (*j.filter == "QUATERNION")
				meshopt_decodeFilterQuat(dst, j.count, j.stride);
			else if (*j.filter == "EXPONENTIAL")
				meshopt_decodeFilterExp(dst, j.count, j.stride);
		}
		success[i] = 1;
	});

	// Rewire successful buffer views
	for (size_t i = 0; i < jobs.size(); i++) {
		const Job& j = jobs[i];
		auto& bv = gltf.bufferViews[j.bv_idx];
		if (!success[i]) {
			VE_LOGW("meshopt decompression failed for buffer view " << j.bv_idx);
			continue;
		}
		bv.buffer = decode_buf_idx;
		bv.byteOffset = j.decode_offset;
		bv.byteLength = j.decoded_size;
		bv.byteStride = (*j.mode == "ATTRIBUTES") ? j.stride : 0;
		bv.extensions.erase("EXT_meshopt_compression");
	}
}

// Detect Blender glTF exporter and return appropriate emissive scale factor.
static float detectEmissiveScale(const tinygltf::Model& gltf) {
	std::string generator_lower = gltf.asset.generator;
	std::transform(generator_lower.begin(), generator_lower.end(), generator_lower.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (generator_lower.find("blender") != std::string::npos) {
		float scale = 1.0f / BLENDER_EMISSIVE_FACTOR;
		VE_LOGI("Blender generator detected, applying emissive scale " << scale);
		return scale;
	}
	return 1.0f;
}

// Register embedded images from a .glb file in the per-load embedded cache.
// Does nothing for .gltf files, which are expected to reference external image files via URI.
static void registerEmbeddedImages(tinygltf::Model& gltf, const std::string& model_path_str,
                                   EmbeddedImageCache& out_cache) {
	for (size_t i = 0; i < gltf.images.size(); i++) {
		auto& img = gltf.images[i];
		if (!img.uri.empty() || img.image.empty())
			continue;
		std::string key = makeEmbeddedKey(model_path_str, static_cast<int>(i));
		bool is_ktx = isKtxMagic(img.image.data(), img.image.size());
		out_cache[key] = EmbeddedImageData{
			std::move(img.image),
			is_ktx
		};
	}
}

// Determine root node indices from the glTF default scene (or fallback to node 0).
static std::vector<int> determineRootNodes(const tinygltf::Model& gltf) {
	std::vector<int> root_nodes;
	if (!gltf.scenes.empty()) {
		int scene_idx = (gltf.defaultScene >= 0 && gltf.defaultScene < static_cast<int>(gltf.scenes.size()))
		                ? gltf.defaultScene : 0;
		root_nodes = gltf.scenes[static_cast<size_t>(scene_idx)].nodes;
	}
	if (root_nodes.empty() && !gltf.nodes.empty())
		root_nodes.push_back(0);
	return root_nodes;
}

// Apply filename-based heuristics to fill in missing texture paths.
static void applyTexturePathFallbacks(
    ParsedMaterial& parsed, const tinygltf::Material& mat,
    const tinygltf::Model& gltf, const std::filesystem::path& model_dir) {
	auto& albedo = parsed.albedo_path;
	auto& normal = parsed.normal_path;
	auto& metallic_roughness = parsed.metallic_roughness_path;
	if (albedo.empty() && !normal.empty()) {
		std::filesystem::path p = tryDerivePath(normal,
			{"_ddna", "_n", "_normal"},
			{"_d", "_c", "_diffuse", "_basecolor", "_albedo"});
		if (!p.empty()) albedo = p;
	}
	if (normal.empty() && !albedo.empty()) {
		std::filesystem::path p = tryDerivePath(albedo,
			{"_d", "_c", "_diffuse", "_basecolor", "_albedo"},
			{"_n", "_normal", "_ddna"});
		if (!p.empty()) normal = p;
	}
	if (metallic_roughness.empty() && !albedo.empty()) {
		std::filesystem::path p = tryDerivePath(albedo,
			{"_d", "_c", "_diffuse", "_basecolor", "_albedo"},
			{"_mr", "_metallic", "_roughness", "_specular"});
		if (!p.empty()) metallic_roughness = p;
	} else if (metallic_roughness.empty() && !normal.empty()) {
		std::filesystem::path p = tryDerivePath(normal,
			{"_ddna", "_n", "_normal"},
			{"_mr", "_metallic", "_roughness", "_specular"});
		if (!p.empty()) metallic_roughness = p;
	}
	// Secondary heuristic: scan glTF images by URI for basecolor/albedo/diffuse + material name match
	if (albedo.empty() && !gltf.images.empty()) {
		std::string mat_name_lower = mat.name;
		if (!mat_name_lower.empty())
			std::transform(mat_name_lower.begin(), mat_name_lower.end(), mat_name_lower.begin(),
			               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		for (size_t img_idx = 0; img_idx < gltf.images.size(); img_idx++) {
			const auto& image = gltf.images[img_idx];
			if (image.uri.empty())
				continue;
			std::string uri_lower = image.uri;
			std::transform(uri_lower.begin(), uri_lower.end(), uri_lower.begin(),
			               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			bool looks_base = (uri_lower.find("basecolor") != std::string::npos || uri_lower.find("albedo") != std::string::npos || uri_lower.find("diffuse") != std::string::npos);
			if (!looks_base)
				continue;
			bool name_match = (!mat_name_lower.empty() && uri_lower.find(mat_name_lower) != std::string::npos);
			if (!name_match) {
				size_t us = uri_lower.find('_');
				if (us != std::string::npos && !mat_name_lower.empty()) {
					std::string prefix = uri_lower.substr(0, us);
					name_match = (mat_name_lower.find(prefix) != std::string::npos);
				}
			}
			if (!name_match)
				continue;
			albedo = model_dir / image.uri;
			break;
		}
	}
}

// Read KHR_texture_transform (offset/scale/rotation) from any glTF texture-info
// type (TextureInfo / NormalTextureInfo / OcclusionTextureInfo all expose
// .extensions). Returns identity when the extension is absent.
template <typename TexInfo>
static UvTransform readUvTransform(const TexInfo& ti, const char* slot, const std::string& model_name) {
	UvTransform xform;
	auto it = ti.extensions.find("KHR_texture_transform");
	if (it == ti.extensions.end())
		return xform;
	const tinygltf::Value& ext = it->second;
	if (ext.Has("offset") && ext.Get("offset").IsArray() && ext.Get("offset").ArrayLen() >= 2) {
		xform.offset.x = static_cast<float>(ext.Get("offset").Get(0).Get<double>());
		xform.offset.y = static_cast<float>(ext.Get("offset").Get(1).Get<double>());
	}
	if (ext.Has("scale") && ext.Get("scale").IsArray() && ext.Get("scale").ArrayLen() >= 2) {
		xform.scale.x = static_cast<float>(ext.Get("scale").Get(0).Get<double>());
		xform.scale.y = static_cast<float>(ext.Get("scale").Get(1).Get<double>());
	}
	if (ext.Has("rotation") && ext.Get("rotation").IsNumber())
		xform.rotation = static_cast<float>(ext.Get("rotation").Get<double>());
	if (ext.Has("texCoord") && ext.Get("texCoord").IsInt() && ext.Get("texCoord").Get<int>() != 0)
		VE_LOGW("glTF '" << model_name << "' " << slot << " KHR_texture_transform uses texCoord set "
			<< ext.Get("texCoord").Get<int>() << "; only TEXCOORD_0 is supported, transform applied to TEXCOORD_0");
	return xform;
}

// Parse a single glTF material into engine-friendly PBR data.
static ParsedMaterial parseSingleMaterial(
    const tinygltf::Material& mat, const tinygltf::Model& gltf,
    const std::filesystem::path& model_dir, const std::string& model_path_str,
    float emissive_scale,
    const EmbeddedImageCache& embedded) {
	ParsedMaterial result;

	// Check for textures
	if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
	    mat.normalTexture.index >= 0 ||
	    mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
		result.has_textures = true;

	auto it_sg = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
	if (it_sg != mat.extensions.end()) {
		const tinygltf::Value& extension = it_sg->second;
		if (extension.Has("diffuseTexture") && extension.Get("diffuseTexture").Has("index"))
			result.has_textures = true;
	}

	// Alpha mode
	if (mat.alphaMode == "BLEND")
		result.alpha_props.alpha_mode = AlphaMode::BLEND;
	else if (mat.alphaMode == "MASK")
		result.alpha_props.alpha_mode = AlphaMode::MASK;
	else
		result.alpha_props.alpha_mode = AlphaMode::ALPHA_OPAQUE;
	result.alpha_props.alpha_cutoff = static_cast<float>(mat.alphaCutoff);
	result.alpha_props.double_sided = mat.doubleSided;

	// PBR factors
	auto& factors = result.factors;
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
	if (it_es != mat.extensions.end() && it_es->second.Has("emissiveStrength") && it_es->second.Get("emissiveStrength").IsNumber())
		factors.emissive_strength = static_cast<float>(it_es->second.Get("emissiveStrength").Get<double>());
	else if (glm::length(factors.emissive_factor) > 0.1f)
		factors.emissive_strength = 1.0f;
	auto it_tr = mat.extensions.find("KHR_materials_transmission");
	if (it_tr != mat.extensions.end() && it_tr->second.Has("transmissionFactor") && it_tr->second.Get("transmissionFactor").IsNumber())
		factors.transmission_factor = static_cast<float>(it_tr->second.Get("transmissionFactor").Get<double>());
	auto it_ior = mat.extensions.find("KHR_materials_ior");
	if (it_ior != mat.extensions.end() && it_ior->second.Has("ior") && it_ior->second.Get("ior").IsNumber()) {
		double ior_val = it_ior->second.Get("ior").Get<double>();
		factors.ior = (ior_val >= 1.0) ? static_cast<float>(ior_val) : 1.5f;
	}
	// KHR_materials_pbrSpecularGlossiness
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
			factors.roughness_factor = std::clamp(glossiness, 0.0f, 1.0f);
		}
		if (ext.Has("specularFactor") && ext.Get("specularFactor").IsArray()) {
			const auto& sf = ext.Get("specularFactor");
			if (sf.Size() >= 3) {
				factors.specular_factor.x = static_cast<float>(sf.Get(0).Get<double>());
				factors.specular_factor.y = static_cast<float>(sf.Get(1).Get<double>());
				factors.specular_factor.z = static_cast<float>(sf.Get(2).Get<double>());
			}
		}
	}

	// KHR_materials_specular (factors only, textures resolved below alongside other tex paths)
	auto it_spec = mat.extensions.find("KHR_materials_specular");
	if (it_spec != mat.extensions.end()) {
		const tinygltf::Value& ext = it_spec->second;
		if (ext.Has("specularFactor") && ext.Get("specularFactor").IsNumber())
			factors.specular_strength = static_cast<float>(ext.Get("specularFactor").Get<double>());
		if (ext.Has("specularColorFactor") && ext.Get("specularColorFactor").IsArray()) {
			const auto& scf = ext.Get("specularColorFactor");
			if (scf.Size() >= 3) {
				factors.specular_factor.x = static_cast<float>(scf.Get(0).Get<double>());
				factors.specular_factor.y = static_cast<float>(scf.Get(1).Get<double>());
				factors.specular_factor.z = static_cast<float>(scf.Get(2).Get<double>());
			}
		}
	}

	// Resolve texture paths
	if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
		result.albedo_path = resolveTexturePath(gltf,
			static_cast<size_t>(mat.pbrMetallicRoughness.baseColorTexture.index), model_dir, model_path_str, embedded);
	} else if (it_sg != mat.extensions.end()) {
		const tinygltf::Value& ext = it_sg->second;
		if (ext.Has("diffuseTexture") && ext.Get("diffuseTexture").Has("index")) {
			int tex_idx = ext.Get("diffuseTexture").Get("index").Get<int>();
			result.albedo_path = resolveTexturePath(gltf, static_cast<size_t>(tex_idx), model_dir, model_path_str, embedded);
		}
	}
	if (mat.normalTexture.index >= 0)
		result.normal_path = resolveTexturePath(gltf, static_cast<size_t>(mat.normalTexture.index), model_dir, model_path_str, embedded);
	if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
		result.metallic_roughness_path = resolveTexturePath(gltf,
			static_cast<size_t>(mat.pbrMetallicRoughness.metallicRoughnessTexture.index), model_dir, model_path_str, embedded);
	} else if (it_sg != mat.extensions.end()) {
		const tinygltf::Value& ext = it_sg->second;
		if (ext.Has("specularGlossinessTexture") && ext.Get("specularGlossinessTexture").Has("index")) {
			int tex_idx = ext.Get("specularGlossinessTexture").Get("index").Get<int>();
			result.metallic_roughness_path = resolveTexturePath(gltf, static_cast<size_t>(tex_idx), model_dir, model_path_str, embedded);
			result.alpha_props.use_spec_gloss_texture = true;
		}
	}
	if (mat.occlusionTexture.index >= 0)
		result.occlusion_path = resolveTexturePath(gltf, static_cast<size_t>(mat.occlusionTexture.index), model_dir, model_path_str, embedded);
	if (mat.emissiveTexture.index >= 0)
		result.emissive_path = resolveTexturePath(gltf, static_cast<size_t>(mat.emissiveTexture.index), model_dir, model_path_str, embedded);

	// KHR_texture_transform per slot (identity when absent)
	result.uv_transforms.albedo = readUvTransform(mat.pbrMetallicRoughness.baseColorTexture, "baseColor", model_path_str);
	result.uv_transforms.metallic_roughness = readUvTransform(mat.pbrMetallicRoughness.metallicRoughnessTexture, "metallicRoughness", model_path_str);
	result.uv_transforms.normal = readUvTransform(mat.normalTexture, "normal", model_path_str);
	result.uv_transforms.occlusion = readUvTransform(mat.occlusionTexture, "occlusion", model_path_str);
	result.uv_transforms.emissive = readUvTransform(mat.emissiveTexture, "emissive", model_path_str);

	// KHR_materials_specular textures
	if (it_spec != mat.extensions.end()) {
		const tinygltf::Value& ext = it_spec->second;
		if (ext.Has("specularTexture") && ext.Get("specularTexture").Has("index")) {
			int tex_idx = ext.Get("specularTexture").Get("index").Get<int>();
			result.specular_path = resolveTexturePath(gltf, static_cast<size_t>(tex_idx), model_dir, model_path_str, embedded);
		}
		if (ext.Has("specularColorTexture") && ext.Get("specularColorTexture").Has("index")) {
			int tex_idx = ext.Get("specularColorTexture").Get("index").Get<int>();
			result.specular_color_path = resolveTexturePath(gltf, static_cast<size_t>(tex_idx), model_dir, model_path_str, embedded);
		}
	}

	// Filename-based fallbacks
	applyTexturePathFallbacks(result, mat, gltf, model_dir);

	return result;
}

// Parse all materials from a glTF model.
// Unbound texture slots stay empty in ParsedMaterial; the upload stage substitutes engine defaults.
static std::vector<ParsedMaterial> parseAllMaterials(
    const tinygltf::Model& gltf, const std::filesystem::path& model_dir,
    const std::string& model_path_str, float emissive_scale,
    const EmbeddedImageCache& embedded) {
	std::vector<ParsedMaterial> results;
	if (!gltf.materials.empty()) {
		results.reserve(gltf.materials.size());
		for (const auto& mat : gltf.materials)
			results.push_back(parseSingleMaterial(mat, gltf, model_dir, model_path_str, emissive_scale, embedded));
	} else {
		results.push_back(ParsedMaterial{});
	}
	return results;
}

// Extract MaterialFactors vector from ParsedMaterial vector (for extractEmissiveLights).
static std::vector<MaterialFactors> extractFactors(const std::vector<ParsedMaterial>& materials) {
	std::vector<MaterialFactors> factors;
	factors.reserve(materials.size());
	for (const auto& m : materials)
		factors.push_back(m.factors);
	return factors;
}

// Some gltf exports flag opaque and cutout surfaces as BLEND
static void reinterpretBlendMaterials(std::vector<ProcessedMaterial>& materials,
                                      const std::vector<DecodedTexture>& textures) {
	for (auto& m : materials) {
		if (m.alpha_props.alpha_mode != AlphaMode::BLEND)
			continue;
		// Genuine translucency (semi-transparent base or transmission) stays BLEND.
		if (m.factors.transmission_factor > 0.0f || m.factors.base_color_factor.w < 0.99f)
			continue;

		// No albedo texture => alpha lives only in the (opaque) base color factor.
		AlphaCoverage coverage = AlphaCoverage::Opaque;
		if (m.albedo_tex_idx >= 0 && static_cast<size_t>(m.albedo_tex_idx) < textures.size())
			coverage = textures[static_cast<size_t>(m.albedo_tex_idx)].alpha_coverage;

		switch (coverage) {
			case AlphaCoverage::Opaque:
				m.alpha_props.alpha_mode = AlphaMode::ALPHA_OPAQUE;
				break;
			case AlphaCoverage::Cutout:
				m.alpha_props.alpha_mode = AlphaMode::MASK;
				if (m.alpha_props.alpha_cutoff <= 0.0f)
					m.alpha_props.alpha_cutoff = 0.5f;
				break;
			// Unknown (decode failure / unsupported format): keep BLEND.
			default:
				break;
		}
	}
}

} // namespace ve

namespace ve::gltf {

// CPU-only loading: no Vulkan calls, thread-safe.
// Parses glTF, decodes textures, processes meshes (MikkTSpace, meshopt, LOD, meshlet).
LoadedAssetData load(
	const std::filesystem::path& model_path,
	bool extract_lights, bool flip_tex_coord_v,
	LoadProgress& progress,
	GpuCaps gpu_caps) {

	LoadedAssetData result;

	// Parse glTF file (lightweight: skips loading external image data)
	tinygltf::Model gltf;
	if (!loadGltfFile(model_path, gltf, /*cpu_only=*/true)) {
		progress.cpu_failed = true;
		return result;
	}
	if (progress.cancelled.load())
		return result;

	decompressMeshopt(gltf);

	std::string model_path_str = model_path.lexically_normal().generic_string();
	registerEmbeddedImages(gltf, model_path_str, result.embedded_images);

	float emissive_scale = detectEmissiveScale(gltf);
	auto parsed_materials = parseAllMaterials(gltf, model_path.parent_path(), model_path_str, emissive_scale, result.embedded_images);

	// 4. Decode all unique textures referenced by materials
	// Collect unique texture paths, decode each once
	struct TexRef { std::filesystem::path path; TextureType type; };
	std::vector<TexRef> tex_refs;
	std::unordered_map<std::string, int> tex_path_to_idx;

	auto addTexRef = [&](const std::filesystem::path& path, TextureType type) -> int {
		if (path.empty())
			return -1;
		std::string key = VeTexture::makeResourceKey(path, type);
		auto it = tex_path_to_idx.find(key);
		if (it != tex_path_to_idx.end())
			return it->second;
		int idx = static_cast<int>(tex_refs.size());
		tex_refs.push_back({path, type});
		tex_path_to_idx[key] = idx;
		return idx;
	};

	// Build ProcessedMaterial array and collect texture references
	result.materials.reserve(parsed_materials.size());
	for (size_t i = 0; i < parsed_materials.size(); i++) {
		const auto& pm = parsed_materials[i];
		ProcessedMaterial pmat;
		pmat.resource_id = model_path.generic_string() + "::material_" + std::to_string(i);
		pmat.alpha_props = pm.alpha_props;
		pmat.factors = pm.factors;
		pmat.uv_transforms = pm.uv_transforms;
		pmat.flip_tex_coord_v = flip_tex_coord_v;
		pmat.albedo_tex_idx = addTexRef(pm.albedo_path, TextureType::ALBEDO);
		pmat.normal_tex_idx = addTexRef(pm.normal_path, TextureType::NORMAL);
		pmat.metallic_roughness_tex_idx = addTexRef(pm.metallic_roughness_path, TextureType::METALLIC_ROUGHNESS);
		pmat.occlusion_tex_idx = addTexRef(pm.occlusion_path, TextureType::OCCLUSION);
		pmat.emissive_tex_idx = addTexRef(pm.emissive_path, TextureType::EMISSIVE);
		pmat.specular_tex_idx = addTexRef(pm.specular_path, TextureType::SPECULAR);
		pmat.specular_color_tex_idx = addTexRef(pm.specular_color_path, TextureType::SPECULAR_COLOR);
		result.materials.push_back(std::move(pmat));
	}

	// Determine root nodes
	std::vector<int> root_nodes = determineRootNodes(gltf);

	// Extract punctual lights
	std::vector<glm::mat4> node_world_engine;
	PosDedup light_pos_dedup;
	if (extract_lights) {
		node_world_engine = computeNodeWorldMatrices(gltf, root_nodes);
		result.punctual_lights = extractPunctualLights(gltf, node_world_engine);
		for (const auto& L : result.punctual_lights)
			light_pos_dedup.insert(quantize(L.position));
	}

	result.cameras = extractCameras(gltf);

	// Discover node graph + unique geometry primitives (serial; fast).
	std::vector<NodePrim> node_primitives;
	std::vector<UniquePrimitive> unique_primitives;
	std::unordered_map<std::string, int> key_to_unique_idx;
	std::vector<PendingMeshAttach> pending_attachments;
	GltfCpuLoadContext cpu_ctx{gltf, model_path_str, result.materials,
	                           node_primitives, unique_primitives,
	                           key_to_unique_idx, pending_attachments, progress};
	for (int root_idx : root_nodes) {
		if (progress.cancelled.load())
			return result;
		discoverNodeCpu(root_idx, -1, cpu_ctx,
		                result.nodes, result.parent_links, result.root_indices,
		                result.gltf_to_loaded_idx);
	}

	// Pre-populate the per-geometry center/extent map; processPrimitive will
	// write into existing slots (safe under concurrent access).
	GeometryCenterExtent geometry_center_extent;
	geometry_center_extent.reserve(unique_primitives.size());
	for (const auto& up : unique_primitives)
		geometry_center_extent.emplace(up.key, std::pair<glm::vec3, float>{glm::vec3(0.f), 0.f});

	// Parallel: decode textures AND process meshes
	const size_t tex_count = tex_refs.size();
	const size_t mesh_count = unique_primitives.size();
	result.textures.resize(tex_count);
	std::vector<ProcessedMesh> raw_meshes(mesh_count);

	// Hand each tex_ref a shared_ptr to its embedded bytes
	std::vector<std::shared_ptr<EmbeddedImageData>> tex_embedded(tex_count);
	{
		std::unordered_map<std::string, std::shared_ptr<EmbeddedImageData>> emb_shared;
		for (auto& [key, data] : result.embedded_images)
			emb_shared.emplace(key, std::make_shared<EmbeddedImageData>(std::move(data)));
		result.embedded_images = EmbeddedImageCache{};
		for (size_t i = 0; i < tex_count; i++) {
			std::string key = tex_refs[i].path.lexically_normal().generic_string();
			if (auto it = emb_shared.find(key); it != emb_shared.end())
				tex_embedded[i] = it->second;
		}
	}

	progress.total_items = static_cast<uint32_t>(tex_count + mesh_count);
	progress.setStatus("Decoding and processing");
	::ve::parallel::forIndexed(tex_count + mesh_count, progress.cancelled,
		[&](size_t i) {
			if (i < tex_count) {
				result.textures[i] = VeTexture::decode(tex_refs[i].path, tex_refs[i].type, gpu_caps, tex_embedded[i].get());
				tex_embedded[i].reset();
			} else {
				size_t mi = i - tex_count;
				const auto& up = unique_primitives[mi];
				raw_meshes[mi] = processPrimitive(*up.primitive, gltf, up.mesh_id,
				                                  &geometry_center_extent.at(up.key));
			}
			progress.completed_items.fetch_add(1, std::memory_order_relaxed);
		});
	if (progress.cancelled.load())
		return result;

	reinterpretBlendMaterials(result.materials, result.textures);

	// Compact non-empty meshes + apply node attachments
	std::unordered_map<std::string, int> geometry_mesh_cache;
	for (size_t i = 0; i < unique_primitives.size(); i++) {
		if (raw_meshes[i].vertices.empty())
			continue;
		int slot = static_cast<int>(result.meshes.size());
		geometry_mesh_cache[unique_primitives[i].key] = slot;
		result.meshes.push_back(std::move(raw_meshes[i]));
	}

	// Mapping loaded node index -> its primitive node indices
	std::unordered_map<uint32_t, std::vector<uint32_t>> weight_mirror_nodes;
	for (const auto& pa : pending_attachments) {
		auto it = geometry_mesh_cache.find(pa.key);
		int mesh_data_idx = (it != geometry_mesh_cache.end()) ? it->second : -1;
		if (mesh_data_idx < 0 || pa.mat_idx >= result.materials.size())
			continue;
		if (pa.prim_idx == 0) {
			result.nodes[pa.node_idx].mesh_idx = mesh_data_idx;
			result.nodes[pa.node_idx].material_idx = static_cast<int>(pa.mat_idx);
		} else {
			// glTF requires all primitives of a mesh to share one weights vector + target count.
			uint32_t parent_tc = (result.nodes[pa.node_idx].mesh_idx >= 0)
				? result.meshes[static_cast<size_t>(result.nodes[pa.node_idx].mesh_idx)].morph_target_count : 0;
			uint32_t this_tc = result.meshes[static_cast<size_t>(mesh_data_idx)].morph_target_count;
			if (this_tc != parent_tc && (this_tc > 0 || parent_tc > 0))
				VE_LOGW("glTF mesh '" << pa.parent_node_name << "' has inconsistent morph target counts across primitives ("
				        << parent_tc << " vs " << this_tc << ")");

			uint32_t prim_node_idx = static_cast<uint32_t>(result.nodes.size());
			result.nodes.push_back({
				.name = pa.parent_node_name + "_prim" + std::to_string(pa.prim_idx),
				.mesh_idx = mesh_data_idx,
				.material_idx = static_cast<int>(pa.mat_idx),
				.skin_idx = pa.skin_idx,
				.morph_weights = result.nodes[pa.node_idx].morph_weights,
			});
			result.parent_links.emplace_back(prim_node_idx, pa.node_idx);
			if (this_tc > 0)
				weight_mirror_nodes[pa.node_idx].push_back(prim_node_idx);
		}
	}

	// Parse animations
	progress.setStatus("Parsing animations");
	result.animation_clips = parseAnimations(gltf, result.gltf_to_loaded_idx, progress);

	// A glTF weights animation targets a mesh node, but we split multi-primitive meshes
	// into seperate nodes per primitive that share the node's weights.
	if (!weight_mirror_nodes.empty()) {
		for (auto& clip : result.animation_clips) {
			size_t channel_count = clip.channels.size();
			for (size_t ci = 0; ci < channel_count; ci++) {
				AnimationChannel ch = clip.channels[ci];
				if (ch.path != AnimationPath::Weights || ch.target_slot >= clip.target_node_indices.size())
					continue;
				auto mirror_it = weight_mirror_nodes.find(clip.target_node_indices[ch.target_slot]);
				if (mirror_it == weight_mirror_nodes.end())
					continue;
				for (uint32_t mirror_node : mirror_it->second) {
					AnimationChannel nch = ch;
					nch.target_slot = static_cast<uint32_t>(clip.target_node_indices.size());
					clip.target_node_indices.push_back(mirror_node);
					clip.channels.push_back(nch);
				}
			}
		}
	}

	result.skins = parseSkins(gltf);

	// Extract emissive lights
	if (extract_lights) {
		result.emissive_lights = extractEmissiveLights(gltf, node_world_engine,
			node_primitives, geometry_center_extent, extractFactors(parsed_materials), light_pos_dedup);
	}

	progress.cpu_done = true;
	VE_LOGI("CPU loading complete for " << model_path << ": "
	        << result.textures.size() << " textures, "
	        << result.meshes.size() << " meshes, "
	        << result.nodes.size() << " nodes");
	return result;
}

} // namespace ve::gltf

namespace ve::gltf {

// Fast path: process only the first primitive of the first mesh.
ResourceHandle<VeMesh> loadFirstMesh(VeResourceManager& rm,
                                     const std::filesystem::path& model_path) {
	tinygltf::Model gltf;
	if (!::ve::loadGltfFile(model_path, gltf, /*cpu_only=*/true))
		return {};
	::ve::decompressMeshopt(gltf);
	for (const auto& mesh : gltf.meshes) {
		for (const auto& prim : mesh.primitives) {
			auto pos_it = prim.attributes.find("POSITION");
			if (pos_it == prim.attributes.end())
				continue;
			std::string mesh_id = model_path.generic_string() + "::first_mesh";
			::ve::ProcessedMesh processed = ::ve::processPrimitive(prim, gltf, mesh_id, nullptr);
			if (processed.vertices.empty())
				continue;
			::ve::SyncUploadScope scope(rm.getDevice());
			return rm.createMeshFromData(processed.resource_id, processed, scope.ctx);
		}
	}
	return {};
}

} // namespace ve::gltf
