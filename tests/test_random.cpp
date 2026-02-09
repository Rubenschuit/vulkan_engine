#include <catch2/catch_test_macros.hpp>
#include <utils/ve_random.hpp>
#include <glm/glm.hpp>
#include <cmath>

static constexpr int NUM_SAMPLES = 200;

TEST_CASE("Random::float01 returns values in [0, 1]", "[utils][random]") {
	for (int i = 0; i < NUM_SAMPLES; ++i) {
		float v = ve::Random::float01();
		REQUIRE(v >= 0.0f);
		REQUIRE(v <= 1.0f);
	}
}

TEST_CASE("Random::floatRange returns values in [min, max]", "[utils][random]") {
	const float min = -5.0f;
	const float max = 10.0f;

	for (int i = 0; i < NUM_SAMPLES; ++i) {
		float v = ve::Random::floatRange(min, max);
		REQUIRE(v >= min);
		REQUIRE(v <= max);
	}
}

TEST_CASE("Random::floatRange single point returns that value", "[utils][random]") {
	for (int i = 0; i < 5; ++i) {
		float v = ve::Random::floatRange(3.14f, 3.14f);
		REQUIRE(v == 3.14f);
	}
}

TEST_CASE("Random::intRange returns values in [min, max] inclusive", "[utils][random]") {
	const int min = -10;
	const int max = 20;

	for (int i = 0; i < NUM_SAMPLES; ++i) {
		int v = ve::Random::intRange(min, max);
		REQUIRE(v >= min);
		REQUIRE(v <= max);
	}
}

TEST_CASE("Random::vec3Range returns components in range", "[utils][random]") {
	const float min = 0.0f;
	const float max = 1.0f;

	for (int i = 0; i < NUM_SAMPLES; ++i) {
		glm::vec3 v = ve::Random::vec3Range(min, max);
		REQUIRE(v.x >= min);
		REQUIRE(v.x <= max);
		REQUIRE(v.y >= min);
		REQUIRE(v.y <= max);
		REQUIRE(v.z >= min);
		REQUIRE(v.z <= max);
	}
}

TEST_CASE("Random::vec3Range min/max returns components in per-axis range", "[utils][random]") {
	glm::vec3 min_val(0.0f, -1.0f, 5.0f);
	glm::vec3 max_val(1.0f, 1.0f, 10.0f);

	for (int i = 0; i < NUM_SAMPLES; ++i) {
		glm::vec3 v = ve::Random::vec3Range(min_val, max_val);
		REQUIRE(v.x >= min_val.x);
		REQUIRE(v.x <= max_val.x);
		REQUIRE(v.y >= min_val.y);
		REQUIRE(v.y <= max_val.y);
		REQUIRE(v.z >= min_val.z);
		REQUIRE(v.z <= max_val.z);
	}
}

TEST_CASE("Random::color returns rgba in valid range", "[utils][random]") {
	for (int i = 0; i < NUM_SAMPLES; ++i) {
		glm::vec4 c = ve::Random::color();
		REQUIRE(c.r >= 0.0f);
		REQUIRE(c.r <= 1.0f);
		REQUIRE(c.g >= 0.0f);
		REQUIRE(c.g <= 1.0f);
		REQUIRE(c.b >= 0.0f);
		REQUIRE(c.b <= 1.0f);
		REQUIRE(c.a == 1.0f);
	}
}

TEST_CASE("Random::colorHSV returns valid rgba", "[utils][random]") {
	for (int i = 0; i < NUM_SAMPLES; ++i) {
		glm::vec4 c = ve::Random::colorHSV();
		REQUIRE(c.r >= 0.0f);
		REQUIRE(c.r <= 1.0f);
		REQUIRE(c.g >= 0.0f);
		REQUIRE(c.g <= 1.0f);
		REQUIRE(c.b >= 0.0f);
		REQUIRE(c.b <= 1.0f);
		REQUIRE(c.a == 1.0f);
	}
}
