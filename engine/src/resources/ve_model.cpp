#include "pch.hpp"
#include "resources/ve_model.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_log.hpp"

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#define GLM_FORCE_RADIANS

namespace ve {

// Custom image loader: KTX/KTX2 files are loaded via VeTexture from URI, not by TinyGLTF.
// Detect KTX magic and return success without calling STB (avoids "Unknown image format" warnings).
// TODO: Make actual KTX/KTX2 loading work (we currently just bypass STB and use VeTexture directly),
// in order to support .glb models with embedded KTX2 textures.
static bool LoadImageDataVeModel(tinygltf::Image* image, const int image_idx, std::string* err,
                                 std::string* warn, int req_width, int req_height,
                                 const unsigned char* bytes, int size, void* user_data) {
	(void)image_idx;
	(void)err;
	(void)warn;
	(void)req_width;
	(void)req_height;
	// KTX magic: 0xAB 0x4B 0x54 0x58 ("KTX")
	// KTX2 magic: 0xAB 0x4B 0x54 0x58 0x20 0x32 0x30 ("KTX 20")
	if (size >= 4 && bytes[0] == 0xAB && bytes[1] == 0x4B && bytes[2] == 0x54 && bytes[3] == 0x58) {
		image->width = image->height = image->component = -1;
		image->bits = image->pixel_type = -1;
		image->as_is = true;
		image->image.clear();
		return true;
	}
	return tinygltf::LoadImageData(image, image_idx, err, warn, req_width, req_height, bytes, size, user_data);
}

// Helper functions to extract information from glTF node.
glm::vec3 getTranslation(const tinygltf::Node& node) {
	if (node.translation.size() >= 3)
		return {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[2]),
		        static_cast<float>(node.translation[1])}; // y-up to z-up
	return {0, 0, 0};
}

glm::vec3 getScale(const tinygltf::Node& node) {
	if (node.scale.size() >= 3)
		return {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[2]),
		        static_cast<float>(node.scale[1])};
	return {1, 1, 1};
}

glm::quat getRotation(const tinygltf::Node& node) {
	if (node.rotation.size() >= 4)
		return {static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
	        static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[1])}; // w,x,y,z, y-up to z-up
	return glm::quat{1, 0, 0, 0};
}

glm::vec3 quatToEuler(const glm::quat& q) {
	return glm::degrees(glm::eulerAngles(q));
}

//----------------------------------
// VeModel implementation
//----------------------------------
std::unique_ptr<VeModel> VeModel::load(VeDevice& device, VeResourceManager& resource_manager,
                                       const std::filesystem::path& model_path,
                                       VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout) {
	auto model = std::make_unique<VeModel>(device);
	model->loadFromGltf(model_path, resource_manager, pool, material_layout);
	return model;
}

VeModel::VeModel(VeDevice& device) : m_ve_device(device) {}

VeModel::~VeModel() = default;

// TODO: add .glb support
void VeModel::loadFromGltf(const std::filesystem::path& model_path, VeResourceManager& resource_manager,
                           VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout) {
	tinygltf::Model gltf;
	tinygltf::TinyGLTF loader;
	// Accept KTX2/other formats as-is: we load textures via VeTexture from URI, not tinygltf's decoded data
	loader.SetImagesAsIs(true);
	// Custom loader: KTX/KTX2 bypass STB (no "Unknown image format" warning); delegate PNG/JPG to default
	tinygltf::LoadImageDataOption load_opt;
	load_opt.as_is = true;
	load_opt.preserve_channels = false;
	loader.SetImageLoader(LoadImageDataVeModel, &load_opt);
	std::string err, warn;
	bool ret = false;
	std::string ext = model_path.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	if (ext == ".glb") {
		VE_LOGI("Loading glTF binary file (NOT SUPPORTED YET): " << model_path);
		ret = loader.LoadBinaryFromFile(&gltf, &err, &warn, model_path.string());
	} else {
		ret = loader.LoadASCIIFromFile(&gltf, &err, &warn, model_path.string());
	}
	if (!ret) {
		VE_LOGE("Failed to load glTF: " << err);
		assert(false);
		return;
	}
	if (!warn.empty())
		VE_LOGW("glTF warning: " << warn);

	// Gather material texture paths
	bool has_textured_materials = false;
	std::vector<std::filesystem::path> albedo_paths, normal_paths, metallic_roughness_paths;
	std::filesystem::path model_dir = model_path.parent_path();

	if (!gltf.materials.empty()) {
		for (const auto& mat : gltf.materials) {
			if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
			    mat.normalTexture.index >= 0 ||
			    mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
				has_textured_materials = true;
				break;
			}
		}
	}
	// Always parse material alpha props from glTF (for models with or without textures)
	m_material_alpha_props.reserve(gltf.materials.size());
	for (const auto& mat : gltf.materials) {
		MaterialAlphaProps props;
		if (mat.alphaMode == "BLEND")
			props.alpha_mode = AlphaMode::BLEND;
		else if (mat.alphaMode == "MASK")
			props.alpha_mode = AlphaMode::MASK;
		else
			props.alpha_mode = AlphaMode::ALPHA_OPAQUE;
		props.alpha_cutoff = static_cast<float>(mat.alphaCutoff);
		props.double_sided = mat.doubleSided;
		m_material_alpha_props.push_back(props);
	}

	if (has_textured_materials) {
		for (const auto& mat : gltf.materials) {
			if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
				size_t tex_idx = static_cast<size_t>(mat.pbrMetallicRoughness.baseColorTexture.index);
				size_t img_idx = static_cast<size_t>(gltf.textures[tex_idx].source);
				albedo_paths.push_back(model_dir / gltf.images[img_idx].uri);
			} else {
				albedo_paths.push_back(model_dir / "default_albedo.png");
			}
			if (mat.normalTexture.index >= 0) {
				size_t tex_idx = static_cast<size_t>(mat.normalTexture.index);
				size_t img_idx = static_cast<size_t>(gltf.textures[tex_idx].source);
				normal_paths.push_back(model_dir / gltf.images[img_idx].uri);
			} else {
				normal_paths.push_back(model_dir / "default_normal.png");
			}
			if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
				size_t tex_idx = static_cast<size_t>(mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
				size_t img_idx = static_cast<size_t>(gltf.textures[tex_idx].source);
				metallic_roughness_paths.push_back(model_dir / gltf.images[img_idx].uri);
			} else {
				metallic_roughness_paths.push_back(model_dir / "default_metallic_roughness.png");
			}
		}
		createPerMaterialTextures(resource_manager, albedo_paths, normal_paths, metallic_roughness_paths);
		if (pool && material_layout) {
			createPerMaterialDescriptorSets(*pool, *material_layout);
		}
		m_has_textured_materials = true;
	}

	std::vector<int> root_nodes;
	if (!gltf.scenes.empty() && gltf.defaultScene >= 0 && gltf.defaultScene < static_cast<int>(gltf.scenes.size())) {
		root_nodes = gltf.scenes[static_cast<size_t>(gltf.defaultScene)].nodes;
	}
	if (root_nodes.empty() && !gltf.nodes.empty()) {
		root_nodes.push_back(0);
	}


	auto createPrimitiveMesh = [&](const tinygltf::Primitive& primitive, const tinygltf::Model& m,
	                              const std::string& mesh_id) -> ResourceHandle<VeMesh> {
		std::vector<VeMesh::Vertex> vertices;
		std::unordered_map<VeMesh::Vertex, uint32_t> unique_vertices;
		std::vector<uint32_t> indices;
		std::vector<uint32_t> primitive_vertex_map;

		const tinygltf::Accessor& pos_accessor = m.accessors[static_cast<size_t>(primitive.attributes.at("POSITION"))];
		const tinygltf::BufferView& pos_bv = m.bufferViews[static_cast<size_t>(pos_accessor.bufferView)];
		const tinygltf::Buffer& pos_buf = m.buffers[static_cast<size_t>(pos_bv.buffer)];

		const tinygltf::Accessor& index_accessor = m.accessors[static_cast<size_t>(primitive.indices)];
		const tinygltf::BufferView& index_bv = m.bufferViews[static_cast<size_t>(index_accessor.bufferView)];
		const tinygltf::Buffer& index_buf = m.buffers[static_cast<size_t>(index_bv.buffer)];

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

		const size_t pos_stride = static_cast<size_t>(pos_accessor.ByteStride(pos_bv) > 0 ? pos_accessor.ByteStride(pos_bv) : 12);
		const size_t normal_stride = static_cast<size_t>(normal_accessor.ByteStride(normal_bv) > 0 ? normal_accessor.ByteStride(normal_bv) : 12);
		const size_t tex_stride = static_cast<size_t>(has_tex_coords && tex_acc->ByteStride(*tex_bv) > 0 ? tex_acc->ByteStride(*tex_bv) : 8);
		const size_t tangent_stride = static_cast<size_t>(has_tangents && tangent_acc->ByteStride(*tangent_bv) > 0 ? tangent_acc->ByteStride(*tangent_bv) : 16);

		for (size_t i = 0; i < pos_accessor.count; i++) {
			VeMesh::Vertex vertex{};
			const float* pos = reinterpret_cast<const float*>(&pos_buf.data[pos_bv.byteOffset + pos_accessor.byteOffset + i * pos_stride]);
			vertex.pos = {pos[0], pos[2], pos[1]};
			vertex.color = {1, 1, 1};
			const float* normal = reinterpret_cast<const float*>(&normal_buf.data[normal_bv.byteOffset + normal_accessor.byteOffset + i * normal_stride]);
			vertex.normal = {normal[0], normal[2], normal[1]};
			if (has_tex_coords && tex_stride > 0) {
				const float* tc = reinterpret_cast<const float*>(&tex_buf->data[tex_bv->byteOffset + tex_acc->byteOffset + i * tex_stride]);
				vertex.tex_coord = {tc[0], tc[1]};
			} else {
				vertex.tex_coord = {0, 0};
			}
			if (has_tangents && tangent_stride > 0) {
				const float* t = reinterpret_cast<const float*>(&tangent_buf->data[tangent_bv->byteOffset + tangent_acc->byteOffset + i * tangent_stride]);
				vertex.tangent = {t[0], t[2], t[1], -t[3]};
			} else {
				vertex.tangent = {0, 0, 0, 0};
			}

			if (!unique_vertices.contains(vertex)) {
				unique_vertices[vertex] = static_cast<uint32_t>(vertices.size());
				vertices.push_back(vertex);
			}
			primitive_vertex_map.push_back(unique_vertices[vertex]);
		}

		const unsigned char* index_data = &index_buf.data[index_bv.byteOffset + index_accessor.byteOffset];
		for (size_t i = 0; i < index_accessor.count; i++) {
			uint32_t accessor_index = 0;
			switch (index_accessor.componentType) {
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
					accessor_index = *reinterpret_cast<const uint8_t*>(index_data + i * sizeof(uint8_t));
					break;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
					accessor_index = *reinterpret_cast<const uint16_t*>(index_data + i * sizeof(uint16_t));
					break;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
					accessor_index = *reinterpret_cast<const uint32_t*>(index_data + i * sizeof(uint32_t));
					break;
				default:
					assert(false);
			}
			indices.push_back(primitive_vertex_map[accessor_index]);
		}

		return resource_manager.createMesh(mesh_id, vertices, indices);
	};

	// Recursively create nodes.
	// Each glTF node induces a GameObject with TransformComponent. If mesh:
	// add MeshComponent(s) to the same object. A gltf node with a mesh can
	// contain multiple primitives (sub-meshes), which are then added as
	// children to the same GameObject.
	// TODO: investigate potential dangling pointers in the parent_links vector when nodes are removed from the scene.
	std::function<void(int, int)> processNode = [&](int gltf_node_idx, int parent_our_idx) {
		const auto& node = gltf.nodes[static_cast<size_t>(gltf_node_idx)];
		glm::vec3 trans = getTranslation(node);
		glm::vec3 scale = getScale(node);
		glm::vec3 rot = quatToEuler(getRotation(node));

		// Create one GameObject for this glTF node (transform + optional mesh)
		VeGameObject node_obj = VeGameObject::createGameObject();
		auto* tr = node_obj.getComponent<TransformComponent>();
		tr->translation = trans;
		tr->scale = scale;
		tr->rotation = rot;

		int node_our_idx = static_cast<int>(m_nodes.size());
		uint32_t node_id = node_obj.getId();
		m_nodes.push_back(std::move(node_obj));

		if (parent_our_idx >= 0) {
			m_parent_links.emplace_back(node_id, m_nodes[static_cast<size_t>(parent_our_idx)].getId());
		} else {
			m_root_ids.insert(node_id);
		}

		// If node has mesh: add first primitive to this object, rest as children
		if (node.mesh >= 0) {
			const auto& mesh = gltf.meshes[static_cast<size_t>(node.mesh)];
			for (size_t prim_idx = 0; prim_idx < mesh.primitives.size(); prim_idx++) {
				const auto& primitive = mesh.primitives[prim_idx];
				uint32_t mat_idx = primitive.material >= 0 ? static_cast<uint32_t>(primitive.material) : 0;
				std::string mesh_id = model_path.generic_string() + "::" + std::to_string(node.mesh) + "::" + std::to_string(prim_idx);
				auto mesh_handle = createPrimitiveMesh(primitive, gltf, mesh_id);
				if (prim_idx == 0) {
					m_nodes[static_cast<size_t>(node_our_idx)].addComponent<MeshComponent>(std::move(mesh_handle), mat_idx);
				} else {
					VeGameObject prim_obj = VeGameObject::createGameObject();
					prim_obj.addComponent<MeshComponent>(std::move(mesh_handle), mat_idx);
					m_parent_links.emplace_back(prim_obj.getId(), node_id);
					m_nodes.push_back(std::move(prim_obj));
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
			m_root_id = m_nodes[0].getId();
		}
	}
	if (!m_nodes.empty() && m_root_id == 0) {
		m_root_id = m_nodes[0].getId();
	}

	VE_LOGI("Loaded model " << model_path << " with " << m_nodes.size() << " nodes");
}

void VeModel::createPerMaterialTextures(VeResourceManager& resource_manager,
                                        const std::vector<std::filesystem::path>& albedo_paths,
                                        const std::vector<std::filesystem::path>& normal_paths,
                                        const std::vector<std::filesystem::path>& metallic_roughness_paths) {
	assert(albedo_paths.size() == normal_paths.size() && normal_paths.size() == metallic_roughness_paths.size());
	m_materials.reserve(albedo_paths.size());
	for (size_t i = 0; i < albedo_paths.size(); i++) {
		Material mat;
		mat.albedo_texture = VeTexture::loadOrDefault(resource_manager, albedo_paths[i], TextureType::ALBEDO, vk::Format::eR8G8B8A8Srgb);
		mat.normal_texture = VeTexture::loadOrDefault(resource_manager, normal_paths[i], TextureType::NORMAL, vk::Format::eR8G8B8A8Unorm);
		mat.metallic_roughness_texture = VeTexture::loadOrDefault(resource_manager, metallic_roughness_paths[i], TextureType::METALLIC_ROUGHNESS, vk::Format::eR8G8B8A8Unorm);
		m_materials.push_back(std::move(mat));
	}
}

void VeModel::createPerMaterialDescriptorSets(VeDescriptorPool& pool, VeDescriptorSetLayout& set_layout) {
	for (auto& mat : m_materials) {
		auto albedo_info = mat.albedo_texture.get()->getDescriptorInfo();
		auto normal_info = mat.normal_texture.get()->getDescriptorInfo();
		auto mr_info = mat.metallic_roughness_texture.get()->getDescriptorInfo();
		vk::raii::DescriptorSet temp{nullptr};
		VeDescriptorWriter(set_layout, pool)
			.writeImage(0, &albedo_info)
			.writeImage(1, &normal_info)
			.writeImage(2, &mr_info)
			.build(temp);
		mat.descriptor_set = std::move(temp);
	}
}

vk::raii::DescriptorSet& VeModel::getMaterialDescriptorSet(uint32_t material_index) {
	assert(material_index < m_materials.size() && m_materials[material_index].descriptor_set && "Invalid material index or descriptor set not created");
	return *m_materials[material_index].descriptor_set;
}

MaterialAlphaProps VeModel::getMaterialAlphaProps(uint32_t material_index) const {
	if (material_index >= m_material_alpha_props.size())
		return {};
	return m_material_alpha_props[material_index];
}

void VeModel::addToScene(std::unordered_map<uint32_t, VeGameObject>& game_objects,
                         const glm::vec3& root_translation,
                         const glm::vec3& root_rotation,
                         const glm::vec3& root_scale) {
	for (auto& node : m_nodes) {
		if (m_root_ids.count(node.getId())) {
			auto* tr = node.getComponent<TransformComponent>();
			tr->translation += root_translation;
			tr->rotation += root_rotation;
			tr->scale *= root_scale;
		}
		if (auto* mesh_comp = node.getComponent<MeshComponent>())
			mesh_comp->setModel(this);
		game_objects.emplace(node.getId(), std::move(node));
	}
	m_nodes.clear();

	for (const auto& [child_id, parent_id] : m_parent_links) {
		if (game_objects.count(child_id) && game_objects.count(parent_id)) {
			game_objects.at(child_id).setParent(&game_objects.at(parent_id));
		}
	}
}

std::vector<VeGameObject> VeModel::addToScene(const glm::vec3& root_translation,
                                              const glm::vec3& root_rotation,
                                              const glm::vec3& root_scale) {
	std::unordered_map<uint32_t, VeGameObject> temp;
	addToScene(temp, root_translation, root_rotation, root_scale);
	std::vector<VeGameObject> result;
	result.reserve(temp.size());
	for (auto& [id, obj] : temp) {
		// Clear model ref: caller may not keep the model (e.g. loadAsSingleObject)
		if (auto* mesh = obj.getComponent<MeshComponent>())
			mesh->setModel(nullptr);
		result.push_back(std::move(obj));
	}
	return result;
}

VeGameObject VeModel::loadAsSingleObject(VeDevice& device, VeResourceManager& resource_manager,
                                        const std::filesystem::path& model_path,
                                        const glm::vec3& translation,
                                        const glm::vec3& rotation,
                                        const glm::vec3& scale) {
	auto model = load(device, resource_manager, model_path.lexically_normal(), nullptr, nullptr);
	auto objects = model->addToScene(translation, rotation, scale);
	// Prefer first object with mesh (in case of transform-only root)
	for (auto& obj : objects) {
		if (obj.getComponent<MeshComponent>())
			return std::move(obj);
	}
	// Fallback: first object
	if (!objects.empty())
		return std::move(objects[0]);
	return VeGameObject::createGameObject();
}

} // namespace ve
