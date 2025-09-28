#include <catch2/catch_test_macros.hpp>
#include <ve_buffer.hpp>

TEST_CASE("VeBuffer::getAlignment rounds up correctly", "[buffer][alignment]") {
    using DS = vk::DeviceSize;

    // No alignment required
    REQUIRE(ve::VeBuffer::getAlignment(DS{1}, DS{0}) == DS{1});
    REQUIRE(ve::VeBuffer::getAlignment(DS{19}, DS{0}) == DS{19});

    // Power-of-two alignment
    REQUIRE(ve::VeBuffer::getAlignment(DS{1}, DS{16}) == DS{16});
    REQUIRE(ve::VeBuffer::getAlignment(DS{16}, DS{16}) == DS{16});
    REQUIRE(ve::VeBuffer::getAlignment(DS{17}, DS{16}) == DS{32});
    REQUIRE(ve::VeBuffer::getAlignment(DS{31}, DS{16}) == DS{32});
    REQUIRE(ve::VeBuffer::getAlignment(DS{32}, DS{16}) == DS{32});

    // The Vulkan guarantees: min{Uniform,Storage}BufferOffsetAlignment are powers-of-two,
    // so non power-of-two cases are not expected in practice. We assert on that internally.
}
