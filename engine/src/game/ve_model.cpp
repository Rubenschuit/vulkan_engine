#include "pch.hpp"
#include "game/ve_model.hpp"
#include "core/ve_descriptors.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

namespace ve {

VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices) : m_ve_device(device) {
	createVertexBuffers(vertices);
}

VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) : m_ve_device(device) {
	createVertexBuffers(vertices);
	createIndexBuffers(indices);
}

// Loads a model from a gltf file and creates vertex and index buffers.
// If the model has material textures, they are loaded and stored in the MaterialTextures struct.
VeModel::VeModel(VeDevice& device, const std::filesystem::path& model_path) : m_ve_device(device) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;
	VE_LOGD("Loading model from " << model_path);

    bool ret = loader.LoadASCIIFromFile(&model, &err, &warn, model_path.string());

    if (!warn.empty())
        VE_LOGW("Model " << model_path << " load warning: " << warn);

    if (!err.empty())
		VE_LOGE("Model " << model_path << " load error: " << err);

	assert(ret && "Failed to load glTF model");
	(void)ret; // no fallback handling for now

	// Check if model has any textured materials
	bool has_textured_materials = false;
	if (!model.materials.empty()) {
		for (const auto& mat : model.materials) {
			if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
				mat.normalTexture.index >= 0 ||
				mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
				has_textured_materials = true;
				break;
			}
		}
	}
	// Gather all the texture paths for the model
	std::vector<std::filesystem::path> albedo_paths;
	std::vector<std::filesystem::path> normal_paths;
	std::vector<std::filesystem::path> metallic_roughness_paths;
	if (has_textured_materials) {
		std::filesystem::path model_dir = model_path.parent_path();

		for (const auto& mat : model.materials) {
			// Albedo/Base Color
			if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
				// safe to cast to size_t because index is non-negative after if check
				size_t tex_idx = static_cast<size_t>(mat.pbrMetallicRoughness.baseColorTexture.index);
				size_t img_idx = static_cast<size_t>(model.textures[tex_idx].source);
				std::string uri = model.images[img_idx].uri;
				albedo_paths.push_back(model_dir / uri);
			} else { // index < 0 means no texture is used
				albedo_paths.push_back(model_dir / "default_albedo.png");
			}

			// Normal Map
			if (mat.normalTexture.index >= 0) {
				size_t tex_idx = static_cast<size_t>(mat.normalTexture.index);
				size_t img_idx = static_cast<size_t>(model.textures[tex_idx].source);
				std::string uri = model.images[img_idx].uri;
				normal_paths.push_back(model_dir / uri);
			} else {
				normal_paths.push_back(model_dir / "default_normal.png");
			}

			// Metallic-Roughness
			if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
				size_t tex_idx = static_cast<size_t>(mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
				size_t img_idx = static_cast<size_t>(model.textures[tex_idx].source);
				std::string uri = model.images[img_idx].uri;
				metallic_roughness_paths.push_back(model_dir / uri);
			} else {
				metallic_roughness_paths.push_back(model_dir / "default_metallic_roughness.png");
			}
		}
	}
	// Log
	/*
	VE_LOGD("Albedo paths: " << albedo_paths.size());
	VE_LOGD("Normal paths: " << normal_paths.size());
	VE_LOGD("Metallic-roughness paths: " << metallic_roughness_paths.size());
	for (const auto& path : albedo_paths) {
		VE_LOGD("Albedo path: " << path);
	}
	for (const auto& path : normal_paths) {
		VE_LOGD("Normal path: " << path);
	}
	for (const auto& path : metallic_roughness_paths) {
		VE_LOGD("Metallic-roughness path: " << path);
	}
	*/

	std::vector<Vertex> vertices; // Global vertices vector, only unique vertices are stored here
	std::unordered_map<Vertex, uint32_t> unique_vertices{}; // Used to track unique vertices
	std::vector<uint32_t> indices; // Contains all indices into the global vertices vector of the model
	for (const auto& mesh : model.meshes) {
		for (const auto& primitive : mesh.primitives) {
			// Local tracking for this primitive
			std::vector<uint32_t> primitive_vertex_map; // Maps accessor index to unique vertex index

			// Get vertex positions
            const tinygltf::Accessor& pos_accessor = model.accessors[static_cast<size_t>(primitive.attributes.at("POSITION"))];
            const tinygltf::BufferView& pos_buffer_view = model.bufferViews[static_cast<size_t>(pos_accessor.bufferView)];
            const tinygltf::Buffer& pos_buffer = model.buffers[static_cast<size_t>(pos_buffer_view.buffer)];

			// Get indices
            const tinygltf::Accessor& index_accessor = model.accessors[static_cast<size_t>(primitive.indices)];
            const tinygltf::BufferView& index_buffer_view = model.bufferViews[static_cast<size_t>(index_accessor.bufferView)];
            const tinygltf::Buffer& index_buffer = model.buffers[static_cast<size_t>(index_buffer_view.buffer)];

			// Get normals
			const tinygltf::Accessor& normal_accessor = model.accessors[static_cast<size_t>(primitive.attributes.at("NORMAL"))];
			const tinygltf::BufferView& normal_buffer_view = model.bufferViews[static_cast<size_t>(normal_accessor.bufferView)];
			const tinygltf::Buffer& normal_buffer = model.buffers[static_cast<size_t>(normal_buffer_view.buffer)];

			// Get tangents if available
			bool has_tangents = primitive.attributes.find("TANGENT") != primitive.attributes.end();
			const tinygltf::Accessor* tangent_accessor = nullptr;
			const tinygltf::BufferView* tangent_buffer_view = nullptr;
			const tinygltf::Buffer* tangent_buffer = nullptr;

			if (has_tangents) {
				tangent_accessor = &model.accessors[static_cast<size_t>(primitive.attributes.at("TANGENT"))];
				tangent_buffer_view = &model.bufferViews[static_cast<size_t>(tangent_accessor->bufferView)];
				tangent_buffer = &model.buffers[static_cast<size_t>(tangent_buffer_view->buffer)];
			}

            // Get texture coordinates if available (only 1 set of tex coords is supported)
            bool has_tex_coords = primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end();
            const tinygltf::Accessor* tex_coord_accessor = nullptr;
            const tinygltf::BufferView* tex_coord_buffer_view = nullptr;
            const tinygltf::Buffer* tex_coord_buffer = nullptr;

			if (has_tex_coords) {
				tex_coord_accessor = &model.accessors[static_cast<size_t>(primitive.attributes.at("TEXCOORD_0"))];
				tex_coord_buffer_view = &model.bufferViews[static_cast<size_t>(tex_coord_accessor->bufferView)];
				tex_coord_buffer = &model.buffers[static_cast<size_t>(tex_coord_buffer_view->buffer)];
			}

			// Get material index of this primitive
			uint32_t material_idx = 0;
			if (primitive.material >= 0 && (static_cast<size_t>(primitive.material) < model.materials.size())) {
				material_idx = static_cast<uint32_t>(primitive.material);
				assert((material_idx == 0 || material_idx < albedo_paths.size()) && "Material index out of bounds");
				assert((material_idx == 0 || material_idx < normal_paths.size()) && "Material index out of bounds");
				assert((material_idx == 0 || material_idx < metallic_roughness_paths.size()) && "Material index out of bounds");
			}

			// Handle vertex data
			for (size_t i = 0; i < pos_accessor.count; i++) {
				Vertex vertex{};

				// Set position (x,y,z)
				const float* pos = reinterpret_cast<const float*>(&pos_buffer.data[pos_buffer_view.byteOffset + pos_accessor.byteOffset + i * 12]);
				vertex.pos = {pos[0], pos[2], pos[1]}; // gltf is right-handed, y-up, we go to z-up

				// Default color
				vertex.color = {1.0f, 1.0f, 1.0f};

				// Set normal
				const float* normal = reinterpret_cast<const float*>(&normal_buffer.data[normal_buffer_view.byteOffset + normal_accessor.byteOffset + i * 12]);
				vertex.normal = glm::vec3{normal[0], normal[2], normal[1]};

				// Set texture coordinates if available
				if (has_tex_coords) {
					const float* tex_coord = reinterpret_cast<const float*>(&tex_coord_buffer->data[tex_coord_buffer_view->byteOffset + tex_coord_accessor->byteOffset + i * 8]);
					vertex.tex_coord = {tex_coord[0], tex_coord[1]};
					if (model_path.string() == "././models/quad.gltf")
						VE_LOGD("Tex coord: " << vertex.tex_coord.x << ", " << vertex.tex_coord.y);
				} else {
					vertex.tex_coord = {0.0f, 0.0f};
				}

				// Set tangent if available
				if (has_tangents) {
					const float* tangent = reinterpret_cast<const float*>(&tangent_buffer->data[tangent_buffer_view->byteOffset + tangent_accessor->byteOffset + i * 16]);
					vertex.tangent = glm::vec4{tangent[0], tangent[2], tangent[1], -tangent[3]};
				} else {
					vertex.tangent = {0.0f, 0.0f, 0.0f, 0.0f};
				}

				// Set material index
				vertex.material_index = material_idx;

				if (!unique_vertices.contains(vertex)) {
					unique_vertices[vertex] = static_cast<uint32_t>(vertices.size());
					vertices.push_back(vertex);
				}
				primitive_vertex_map.push_back(unique_vertices[vertex]);
			}

			// Handle index data
			const unsigned char* index_data = &index_buffer.data[index_buffer_view.byteOffset + index_accessor.byteOffset];
			indices.reserve(indices.size() + index_accessor.count);

			// Loop through index_data and add all indices to the indices vector depending on the component type
			// These indices refer to the indices of the global vertices vector WITH duplicates
			for (size_t i = 0; i < index_accessor.count; i++) {
				uint32_t accessor_index = 0;
				switch (index_accessor.componentType) {
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
						accessor_index = *reinterpret_cast<const uint8_t*>(index_data + i * sizeof(uint8_t));
						break;
					}
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
						accessor_index = *reinterpret_cast<const uint16_t*>(index_data + i * sizeof(uint16_t));
						break;
					}
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
						accessor_index = *reinterpret_cast<const uint32_t*>(index_data + i * sizeof(uint32_t));
						break;
					}
					default: {
						VE_LOGE("Unsupported index component type in model " << model_path);
						assert(false);
					}
				}
				// Map accessor index to deduplicated vertex index
				indices.push_back(primitive_vertex_map[accessor_index]);
			}
		} // for primitive
	} // for mesh

	VE_LOGI("Model " << model_path << " has " << vertices.size() << " vertices and " << indices.size() << " indices");
	createVertexBuffers(vertices);
	createIndexBuffers(indices);
	if (has_textured_materials) {
		createMaterialTextures(albedo_paths, normal_paths, metallic_roughness_paths);
	}
}

VeModel::~VeModel() {}

void VeModel::createVertexBuffers(const std::vector<Vertex>& vertices) {
	m_vertex_count = static_cast<uint32_t>(vertices.size());
	assert(m_vertex_count >= 3 && "Vertex count must be at least 3!");

	// Create a local scope staging buffer, accessible by CPU
	ve::VeBuffer staging_buffer(
		m_ve_device,
		sizeof(vertices[0]),
		m_vertex_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Copy vertex data to staging buffer
	staging_buffer.map();
	staging_buffer.writeToBuffer((void*)vertices.data());
	// unmap is called in the destructor of VeBuffer

	// Create vertex buffer, accessible by GPU only
	m_vertex_buffer = std::make_unique<ve::VeBuffer>(
		m_ve_device,
		sizeof(vertices[0]),
		m_vertex_count,
		vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);

	// Copy vertex data from staging buffer to vertex buffer
	auto buffer_size = sizeof(vertices[0]) * m_vertex_count;
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_vertex_buffer->getBuffer(), buffer_size);
}

void VeModel::createIndexBuffers(const std::vector<uint32_t>& indices) {
	m_index_count = static_cast<uint32_t>(indices.size());
	assert(m_index_count >= 3 && "Index count must be at least 3!");

	// Create a local scope staging buffer, accessible by CPU
	ve::VeBuffer staging_buffer(
		m_ve_device,
		sizeof(indices[0]),
		m_index_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Copy index data to staging buffer
	staging_buffer.map();
	staging_buffer.writeToBuffer((void*)indices.data());
	// unmap is called in the destructor of VeBuffer

	// Create index buffer, accessible by GPU only
	m_index_buffer = std::make_unique<ve::VeBuffer>(
		m_ve_device,
		sizeof(indices[0]),
		m_index_count,
		vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);

	// Copy index data from staging buffer to index buffer
	auto buffer_size = sizeof(indices[0]) * m_index_count;
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_index_buffer->getBuffer(), buffer_size);
}

// Creates material textures for a pbr model
void VeModel::createMaterialTextures(
		const std::vector<std::filesystem::path>& albedo_paths,
		const std::vector<std::filesystem::path>& normal_paths,
		const std::vector<std::filesystem::path>& metallic_roughness_paths ) {
	assert(!albedo_paths.empty() && "Albedo paths must not be empty");
	assert(!normal_paths.empty() && "Normal paths must not be empty");
	assert(!metallic_roughness_paths.empty() && "Metallic-roughness paths must not be empty");
	m_material_textures.albedo_texture = std::make_shared<VeTexture>(m_ve_device, albedo_paths, vk::Format::eR8G8B8A8Srgb);
	m_material_textures.normal_texture = std::make_shared<VeTexture>(m_ve_device, normal_paths, vk::Format::eR8G8B8A8Unorm);
	m_material_textures.metallic_roughness_texture = std::make_shared<VeTexture>(m_ve_device, metallic_roughness_paths, vk::Format::eR8G8B8A8Unorm);
}

// Creates a descriptor set for a pbr model with textures
void VeModel::createDescriptorSet(VeDescriptorPool& pool, VeDescriptorSetLayout& set_layout) {
	auto albedo_info = m_material_textures.albedo_texture->getDescriptorInfo();
	auto normal_info = m_material_textures.normal_texture->getDescriptorInfo();
	auto metallic_roughness_info = m_material_textures.metallic_roughness_texture->getDescriptorInfo();
	m_material_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(set_layout, pool)
		.writeImage(0, &albedo_info)
		.writeImage(1, &normal_info)
		.writeImage(2, &metallic_roughness_info)
		.build(m_material_descriptor_set);
	VE_LOGD("Created material descriptor set for model " );
	m_has_textured_materials = true;
}

void VeModel::bindVertexBuffer(vk::raii::CommandBuffer& command_buffer) {
	vk::Buffer buffers[] = { *m_vertex_buffer->getBuffer() };
	vk::DeviceSize offsets[] = { 0 };
	command_buffer.bindVertexBuffers(0, buffers, offsets);
}

void VeModel::bindIndexBuffer(vk::raii::CommandBuffer& command_buffer) {
	command_buffer.bindIndexBuffer(*m_index_buffer->getBuffer(), 0, vk::IndexType::eUint32);
}

void VeModel::draw(vk::raii::CommandBuffer& command_buffer) {
	command_buffer.draw(m_vertex_count, 1, 0, 0);
}

void VeModel::drawIndexed(vk::raii::CommandBuffer& command_buffer) {
	command_buffer.drawIndexed(m_index_count, 1, 0, 0, 0);
}

std::vector<vk::VertexInputBindingDescription> VeModel::Vertex::getBindingDescriptions() {
	std::vector<vk::VertexInputBindingDescription> binding_descriptions(1);
	binding_descriptions[0] = vk::VertexInputBindingDescription{
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = vk::VertexInputRate::eVertex
	};
	return binding_descriptions;
}

std::vector<vk::VertexInputAttributeDescription> VeModel::Vertex::getAttributeDescriptionsSimple() {
	std::vector<vk::VertexInputAttributeDescription> attribute_descriptions(4);
	attribute_descriptions[0] = vk::VertexInputAttributeDescription{
		.location = 0,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, pos),
	};
	attribute_descriptions[1] = vk::VertexInputAttributeDescription{
		.location = 1,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, color)
	};
	attribute_descriptions[2] = vk::VertexInputAttributeDescription{
		.location = 2,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, normal)
	};
	attribute_descriptions[3] = vk::VertexInputAttributeDescription{
		.location = 3,
		.binding = 0,
		.format = vk::Format::eR32G32Sfloat,
		.offset = offsetof(Vertex, tex_coord)
	};
	return attribute_descriptions;

}

// order is important for the shader
std::vector<vk::VertexInputAttributeDescription> VeModel::Vertex::getAttributeDescriptions() {
	std::vector<vk::VertexInputAttributeDescription> attribute_descriptions(6);
	attribute_descriptions[0] = vk::VertexInputAttributeDescription{
		.location = 0,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, pos),
	};
	attribute_descriptions[1] = vk::VertexInputAttributeDescription{
		.location = 1,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, color)
	};
	attribute_descriptions[2] = vk::VertexInputAttributeDescription{
		.location = 2,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, normal)
	};
	attribute_descriptions[3] = vk::VertexInputAttributeDescription{
		.location = 3,
		.binding = 0,
		.format = vk::Format::eR32G32Sfloat,
		.offset = offsetof(Vertex, tex_coord)
	};
	attribute_descriptions[4] = vk::VertexInputAttributeDescription{
		.location = 4,
		.binding = 0,
		.format = vk::Format::eR32G32B32A32Sfloat,
		.offset = offsetof(Vertex, tangent)
	};
	attribute_descriptions[5] = vk::VertexInputAttributeDescription{
		.location = 5,
		.binding = 0,
		.format = vk::Format::eR32Uint,
		.offset = offsetof(Vertex, material_index)
	};

	return attribute_descriptions;
}

std::vector<vk::VertexInputAttributeDescription> VeModel::Vertex::getAttributeDescriptionsShadow() {
	std::vector<vk::VertexInputAttributeDescription> attribute_descriptions(1);
	attribute_descriptions[0] = vk::VertexInputAttributeDescription{
		.location = 0,
		.binding = 0,
		.format = vk::Format::eR32G32B32Sfloat,
		.offset = offsetof(Vertex, pos)
	};
	return attribute_descriptions;
}
}