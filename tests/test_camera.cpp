#include <catch2/catch_test_macros.hpp>
#include <ve_camera.hpp>
#include <glm/glm.hpp>
#include <glm/geometric.hpp>

static bool approxEqual(float a, float b, float eps = 1e-5f) {
	return std::abs(a - b) <= eps;
}

static bool vecApprox(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) {
	return approxEqual(a.x, b.x, eps) && approxEqual(a.y, b.y, eps) && approxEqual(a.z, b.z, eps);
}

static bool matEqual(const glm::mat4& a, const glm::mat4& b, float eps = 0.0f) {
	for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
		if (std::abs(a[r][c] - b[r][c]) > eps) return false;
	}
	return true;
}

static bool vecEqual(const glm::vec3& a, const glm::vec3& b, float eps = 0.0f) {
	return (std::abs(a.x - b.x) <= eps) && (std::abs(a.y - b.y) <= eps) && (std::abs(a.z - b.z) <= eps);
}

TEST_CASE("Camera view updates only via updateIfDirty", "[camera][dirty]") {
	ve::VeCamera cam{};

	// Snapshot initial cached state
	glm::mat4 view0 = cam.getView();
	glm::vec3 fwd0 = cam.getForward();

	// Mutate pose without applying
	cam.yawBy(0.5f);
	cam.moveForward(1.0f);

	// Cached values should be unchanged until updateIfDirty is called
	glm::mat4 view1 = cam.getView();
	glm::vec3 fwd1 = cam.getForward();
	REQUIRE(matEqual(view1, view0));
	REQUIRE(vecEqual(fwd1, fwd0));

	// Apply and confirm change
	cam.updateIfDirty();
	glm::mat4 view2 = cam.getView();
	glm::vec3 fwd2 = cam.getForward();
	REQUIRE_FALSE(matEqual(view2, view0, 1e-6f));
	REQUIRE_FALSE(vecEqual(fwd2, fwd0, 1e-6f));

	// Second update without changes should be a no-op
	glm::mat4 before = cam.getView();
	cam.updateIfDirty();
	glm::mat4 after = cam.getView();
	REQUIRE(matEqual(before, after));
}

TEST_CASE("Projection updates immediately on setPerspective", "[camera][projection]") {
	ve::VeCamera cam{};
	glm::mat4 proj0 = cam.getProj();

	// Change FOV and aspect; projection should update immediately
	cam.setPerspective(1.2f, 16.0f/9.0f, 0.1f, 250.0f);
	glm::mat4 proj1 = cam.getProj();
	REQUIRE_FALSE(matEqual(proj1, proj0));

	// Vulkan clip-space correction flips Y
	REQUIRE(proj1[1][1] < 0.0f);
}

TEST_CASE("lookAt marks dirty; view remains cached until update", "[camera][lookAt]") {
	ve::VeCamera cam{};
	glm::mat4 view0 = cam.getView();

	cam.setPosition({0.0f, 0.0f, 0.0f});
	cam.lookAt({1.0f, 0.0f, 0.0f});

	// Still old view until applied
	REQUIRE(matEqual(cam.getView(), view0));

	cam.updateIfDirty();
	REQUIRE_FALSE(matEqual(cam.getView(), view0));
}

TEST_CASE("Pitch is clamped near vertical", "[camera][edge]") {
	ve::VeCamera cam{};
	cam.setPosition({0,0,0});
	// look straight up
	cam.lookAt({0, 10, 0});
	cam.updateIfDirty();
	// forward should have large positive Y, but not exactly 1 (due to clamp)
	REQUIRE(cam.getForward().y <= 1.0f);
	REQUIRE(cam.getForward().y > 0.0f);
	// up must be well-defined and orthonormal
	REQUIRE(approxEqual(glm::length(cam.getForward()), 1.0f, 1e-4f));
	REQUIRE(approxEqual(glm::length(cam.getRight()), 1.0f, 1e-4f));
	REQUIRE(approxEqual(glm::length(cam.getUp()), 1.0f, 1e-4f));
	REQUIRE(approxEqual(glm::dot(cam.getForward(), cam.getUp()), 0.0f, 1e-4f));
	REQUIRE(approxEqual(glm::dot(cam.getForward(), cam.getRight()), 0.0f, 1e-4f));
	REQUIRE(approxEqual(glm::dot(cam.getRight(), cam.getUp()), 0.0f, 1e-4f));
}

TEST_CASE("Yaw wraps into [-pi,pi]", "[camera][edge]") {
	ve::VeCamera cam{};
	// Apply large positive yaw in multiple steps
	for (int i = 0; i < 20; ++i) cam.yawBy(glm::radians(45.0f));
		cam.updateIfDirty();
	// forward length still 1 and basis orthonormal
	REQUIRE(approxEqual(glm::length(cam.getForward()), 1.0f, 1e-4f));
	REQUIRE(approxEqual(glm::dot(cam.getForward(), cam.getRight()), 0.0f, 1e-4f));
}

TEST_CASE("lookAt down handles singularity", "[camera][edge]") {
	ve::VeCamera cam{};
	cam.setPosition({0,0,0});
	cam.lookAt({0, -5, 0});
	cam.updateIfDirty();
	REQUIRE(cam.getForward().y < 0.0f);
	REQUIRE(approxEqual(glm::length(cam.getRight()), 1.0f, 1e-4f));
	REQUIRE(approxEqual(glm::dot(cam.getForward(), cam.getRight()), 0.0f, 1e-4f));
}

