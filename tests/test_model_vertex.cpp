#include <catch2/catch_test_macros.hpp>
#include <resources/ve_mesh.hpp>
#include <vulkan/vulkan.hpp>

TEST_CASE("VeMesh::Vertex binding description", "[mesh][vertex]") {
    auto bindings = ve::VeMesh::Vertex::getBindingDescriptions();

    REQUIRE(bindings.size() == 1);
    REQUIRE(bindings[0].binding == 0);
    REQUIRE(bindings[0].stride == sizeof(ve::VeMesh::Vertex));
    REQUIRE(bindings[0].inputRate == vk::VertexInputRate::eVertex);
}

TEST_CASE("VeMesh::Vertex attribute descriptions", "[mesh][vertex]") {
    auto attributes = ve::VeMesh::Vertex::getAttributeDescriptions();

    // Expected locations:
    // 0: Position (vec3)
    // 1: Color (vec3)
    // 2: Normal (vec3)
    // 3: TexCoord (vec2)
    // 4: Tangent (vec4)
    // Material is now per-draw via descriptor set, not per-vertex

    REQUIRE(attributes.size() == 5);

    // Position
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeMesh::Vertex, pos));

    // Color
    REQUIRE(attributes[1].binding == 0);
    REQUIRE(attributes[1].location == 1);
    REQUIRE(attributes[1].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[1].offset == offsetof(ve::VeMesh::Vertex, color));

    // Normal
    REQUIRE(attributes[2].binding == 0);
    REQUIRE(attributes[2].location == 2);
    REQUIRE(attributes[2].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[2].offset == offsetof(ve::VeMesh::Vertex, normal));

    // TexCoord
    REQUIRE(attributes[3].binding == 0);
    REQUIRE(attributes[3].location == 3);
    REQUIRE(attributes[3].format == vk::Format::eR32G32Sfloat);
    REQUIRE(attributes[3].offset == offsetof(ve::VeMesh::Vertex, tex_coord));

    // Tangent
    REQUIRE(attributes[4].binding == 0);
    REQUIRE(attributes[4].location == 4);
    REQUIRE(attributes[4].format == vk::Format::eR32G32B32A32Sfloat);
    REQUIRE(attributes[4].offset == offsetof(ve::VeMesh::Vertex, tangent));
}

TEST_CASE("VeMesh::Vertex attribute descriptions (Shadow)", "[mesh][vertex]") {
    auto attributes = ve::VeMesh::Vertex::getAttributeDescriptionsShadow();

    REQUIRE(attributes.size() == 1);
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeMesh::Vertex, pos));
}

TEST_CASE("VeMesh::Vertex attribute descriptions (Simple)", "[mesh][vertex]") {
    auto attributes = ve::VeMesh::Vertex::getAttributeDescriptionsSimple();

    REQUIRE(attributes.size() == 4);
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeMesh::Vertex, pos));

    REQUIRE(attributes[1].location == 1);
    REQUIRE(attributes[1].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[1].offset == offsetof(ve::VeMesh::Vertex, color));

    REQUIRE(attributes[2].location == 2);
    REQUIRE(attributes[2].offset == offsetof(ve::VeMesh::Vertex, normal));

    REQUIRE(attributes[3].location == 3);
    REQUIRE(attributes[3].format == vk::Format::eR32G32Sfloat);
    REQUIRE(attributes[3].offset == offsetof(ve::VeMesh::Vertex, tex_coord));
}

