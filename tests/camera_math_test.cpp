#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <scene/camera_math.hpp>
#include <scene/fly_camera_controller.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

using Catch::Approx;

TEST_CASE("forwardFromYawPitch encodes the Z-up, yaw-0-is-minus-X convention", "[camera][math]") {
	glm::vec3 f = ve::forwardFromYawPitch(0.0f, 0.0f);
	REQUIRE(f.x == Approx(-1.0f).margin(1e-5));
	REQUIRE(f.y == Approx(0.0f).margin(1e-5));
	REQUIRE(f.z == Approx(0.0f).margin(1e-5));

	// Yaw increases toward +Y
	f = ve::forwardFromYawPitch(glm::half_pi<float>(), 0.0f);
	REQUIRE(f.x == Approx(0.0f).margin(1e-5));
	REQUIRE(f.y == Approx(1.0f).margin(1e-5));

	// Positive pitch looks up
	f = ve::forwardFromYawPitch(0.0f, glm::half_pi<float>());
	REQUIRE(f.z == Approx(1.0f).margin(1e-5));
}

TEST_CASE("yawPitchFromForward round-trips forwardFromYawPitch", "[camera][math]") {
	for (int yi = -7; yi <= 7; yi++) {
		for (int pi_ = -8; pi_ <= 8; pi_++) {
			float yaw = static_cast<float>(yi) * (glm::pi<float>() / 8.0f);
			float pitch = static_cast<float>(pi_) * glm::radians(10.0f);
			ve::wrapYaw(yaw); // keep in (-pi, pi] so the comparison is meaningful

			glm::vec2 yp = ve::yawPitchFromForward(ve::forwardFromYawPitch(yaw, pitch));
			REQUIRE(yp.y == Approx(pitch).margin(1e-4));
			// Near-vertical pitch makes yaw degenerate; skip the yaw check there.
			if (std::abs(std::cos(pitch)) > 1e-3f)
				REQUIRE(yp.x == Approx(yaw).margin(1e-4));
		}
	}
}

TEST_CASE("yawPitchFromForward keeps the fallback yaw when the direction is degenerate", "[camera][math]") {
	glm::vec2 yp = ve::yawPitchFromForward(glm::vec3(0.0f), 1.234f);
	REQUIRE(yp.x == Approx(1.234f));
	REQUIRE(yp.y == Approx(0.0f));

	// Straight up: pitch is well defined, yaw is not.
	yp = ve::yawPitchFromForward(glm::vec3(0.0f, 0.0f, 1.0f), 0.5f);
	REQUIRE(yp.x == Approx(0.5f));
	REQUIRE(yp.y == Approx(glm::half_pi<float>()).margin(1e-4));
}

TEST_CASE("wrapYaw and the pitch clamps", "[camera][math]") {
	float y = glm::pi<float>() * 1.5f;
	ve::wrapYaw(y);
	REQUIRE(y == Approx(-glm::half_pi<float>()).margin(1e-5));

	y = -glm::pi<float>() * 1.5f;
	ve::wrapYaw(y);
	REQUIRE(y == Approx(glm::half_pi<float>()).margin(1e-5));

	float p = glm::radians(120.0f);
	ve::clampPitch(p);
	REQUIRE(p == Approx(glm::radians(89.0f)));
	p = glm::radians(-120.0f);
	ve::clampPitch(p);
	REQUIRE(p == Approx(glm::radians(-89.0f)));

	// Asymmetric range, as the orbit camera needs
	p = glm::radians(80.0f);
	ve::clampPitchRange(p, glm::radians(-70.0f), glm::radians(60.0f));
	REQUIRE(p == Approx(glm::radians(60.0f)));
	p = glm::radians(-80.0f);
	ve::clampPitchRange(p, glm::radians(-70.0f), glm::radians(60.0f));
	REQUIRE(p == Approx(glm::radians(-70.0f)));
}

// Guards the camera_math.hpp extraction: lookAt must still agree with the shared
// helper it was factored into.
TEST_CASE("FlyCameraController::lookAt matches yawPitchFromForward", "[camera][math]") {
	const glm::vec3 eye{3.0f, -2.0f, 4.0f};
	const glm::vec3 targets[] = {
		{10.0f, 0.0f, 4.0f}, {-5.0f, 5.0f, 0.0f}, {3.0f, -2.0f, 40.0f}, {0.0f, 0.0f, 0.0f},
	};

	for (const glm::vec3& t : targets) {
		ve::FlyCameraController cam;
		cam.setPosition(eye);
		cam.lookAt(t);

		glm::vec2 yp = ve::yawPitchFromForward(t - eye);
		ve::clampPitch(yp.y);
		REQUIRE(cam.yaw() == Approx(yp.x).margin(1e-4));
		REQUIRE(cam.pitch() == Approx(yp.y).margin(1e-4));

		// forward() must point at the target (unless the clamp kicked in)
		if (std::abs(yp.y) < glm::radians(89.0f)) {
			glm::vec3 want = glm::normalize(t - eye);
			glm::vec3 got = cam.forward();
			REQUIRE(glm::dot(want, got) == Approx(1.0f).margin(1e-4));
		}
	}
}
