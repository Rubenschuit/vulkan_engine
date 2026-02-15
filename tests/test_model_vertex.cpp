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
    // 1: Normal (vec3)
    // 2: TexCoord (vec2)
    // 3: Tangent (vec4)

    REQUIRE(attributes.size() == 4);

    // Position
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeMesh::Vertex, pos));

    // Normal
    REQUIRE(attributes[1].binding == 0);
    REQUIRE(attributes[1].location == 1);
    REQUIRE(attributes[1].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[1].offset == offsetof(ve::VeMesh::Vertex, normal));

    // TexCoord
    REQUIRE(attributes[2].binding == 0);
    REQUIRE(attributes[2].location == 2);
    REQUIRE(attributes[2].format == vk::Format::eR32G32Sfloat);
    REQUIRE(attributes[2].offset == offsetof(ve::VeMesh::Vertex, tex_coord));

    // Tangent
    REQUIRE(attributes[3].binding == 0);
    REQUIRE(attributes[3].location == 3);
    REQUIRE(attributes[3].format == vk::Format::eR32G32B32A32Sfloat);
    REQUIRE(attributes[3].offset == offsetof(ve::VeMesh::Vertex, tangent));
}

TEST_CASE("VeMesh::Vertex attribute descriptions (Shadow)", "[mesh][vertex]") {
    auto attributes = ve::VeMesh::Vertex::getAttributeDescriptionsShadow();

    REQUIRE(attributes.size() == 1);
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == 0);
}

TEST_CASE("VeMesh::Vertex shadow binding description", "[mesh][vertex]") {
    auto bindings = ve::VeMesh::Vertex::getShadowBindingDescriptions();

    REQUIRE(bindings.size() == 1);
    REQUIRE(bindings[0].binding == 0);
    REQUIRE(bindings[0].stride == sizeof(glm::vec3));
    REQUIRE(bindings[0].inputRate == vk::VertexInputRate::eVertex);
}

TEST_CASE("VeMesh::Vertex attribute descriptions (Simple)", "[mesh][vertex]") {
    auto attributes = ve::VeMesh::Vertex::getAttributeDescriptionsSimple();

    REQUIRE(attributes.size() == 3);
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeMesh::Vertex, pos));

    REQUIRE(attributes[1].location == 1);
    REQUIRE(attributes[1].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[1].offset == offsetof(ve::VeMesh::Vertex, normal));

    REQUIRE(attributes[2].location == 2);
    REQUIRE(attributes[2].format == vk::Format::eR32G32Sfloat);
    REQUIRE(attributes[2].offset == offsetof(ve::VeMesh::Vertex, tex_coord));
}
