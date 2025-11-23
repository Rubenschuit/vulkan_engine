#include <catch2/catch_test_macros.hpp>
#include <game/ve_model.hpp>
#include <vulkan/vulkan.hpp>

TEST_CASE("VeModel::Vertex binding description", "[model][vertex]") {
    auto bindings = ve::VeModel::Vertex::getBindingDescriptions();

    REQUIRE(bindings.size() == 1);
    REQUIRE(bindings[0].binding == 0);
    REQUIRE(bindings[0].stride == sizeof(ve::VeModel::Vertex));
    REQUIRE(bindings[0].inputRate == vk::VertexInputRate::eVertex);
}

TEST_CASE("VeModel::Vertex attribute descriptions", "[model][vertex]") {
    auto attributes = ve::VeModel::Vertex::getAttributeDescriptions();

    // Expected locations:
    // 0: Position (vec3)
    // 1: Color (vec3)
    // 2: Normal (vec3)
    // 3: TexCoord (vec2)
    // 4: Tangent (vec4)

    REQUIRE(attributes.size() >= 5); // At least these 5

    // Position
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeModel::Vertex, pos));

    // Color
    REQUIRE(attributes[1].binding == 0);
    REQUIRE(attributes[1].location == 1);
    REQUIRE(attributes[1].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[1].offset == offsetof(ve::VeModel::Vertex, color));

    // Normal
    REQUIRE(attributes[2].binding == 0);
    REQUIRE(attributes[2].location == 2);
    REQUIRE(attributes[2].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[2].offset == offsetof(ve::VeModel::Vertex, normal));

    // TexCoord
    REQUIRE(attributes[3].binding == 0);
    REQUIRE(attributes[3].location == 3);
    REQUIRE(attributes[3].format == vk::Format::eR32G32Sfloat);
    REQUIRE(attributes[3].offset == offsetof(ve::VeModel::Vertex, tex_coord));

    // Tangent
    REQUIRE(attributes[4].binding == 0);
    REQUIRE(attributes[4].location == 4);
    REQUIRE(attributes[4].format == vk::Format::eR32G32B32A32Sfloat);
    REQUIRE(attributes[4].offset == offsetof(ve::VeModel::Vertex, tangent));
}

TEST_CASE("VeModel::Vertex attribute descriptions (Shadow)", "[model][vertex]") {
    auto attributes = ve::VeModel::Vertex::getAttributeDescriptionsShadow();

    REQUIRE(attributes.size() == 1);
    REQUIRE(attributes[0].binding == 0);
    REQUIRE(attributes[0].location == 0);
    REQUIRE(attributes[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(attributes[0].offset == offsetof(ve::VeModel::Vertex, pos));
}

