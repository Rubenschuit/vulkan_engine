/* Class representing a camera in 3D space, it controls the view and projection matrices */
#pragma once

#include <glm/glm.hpp>

namespace ve {

	class VeCamera {
	public:
		// World up defaults to +Y, position defaults to (2,2,2)
		// Looking down -X axis, yaw=0, pitch=0
		// FOV 55 degrees, aspect 4:3, near 0.1, far 100
		explicit VeCamera(glm::vec3 position = {2.0f, 2.0f, 2.0f}, glm::vec3 world_up = {0.0f, 1.0f, 0.0f});

		void setPosition(const glm::vec3& p);

		const glm::vec3& getPosition() const { return pos; }

		void setYawPitch(float yaw_rad, float pitch_rad);
		void yawBy(float delta_rad);
		void pitchBy(float delta_rad);

		// Point the camera at a target position in world space
		void lookAt(const glm::vec3& target);

		// Movement in camera basis/world up
		void moveForward(float d);
		void moveRight(float d);
		void moveUpWorld(float d);

		// Set aspect/FOV/clip
		void setPerspective(float fov_y_radians, float aspect, float near_z, float far_z);

		void updateIfDirty();

		const glm::mat4& getView() const { return view; }
		const glm::mat4& getProj() const { return proj; }
		const glm::vec3& getForward() const { return forward; }
		const glm::vec3& getRight() const { return right; }
		const glm::vec3& getUp() const { return up; }

	private:
		void clampPitch();
		void wrapYaw();
		void updateView();
		void updateProjection();

		glm::vec3 pos;
		glm::vec3 world_up{0.0f, 1.0f, 0.0f};
		float yaw{0.0f};   // radians
		float pitch{0.0f}; // radians
		bool viewDirty{true};

		// Derived
		glm::vec3 forward{1,0,0};
		glm::vec3 right{0,0,1};
		glm::vec3 up{0,1,0};
		glm::mat4 view{1.0f};

		// Projection
		float fovY{glm::radians(55.0f)};
		float aspectRatio{4.0f/3.0f};
		float zNear{0.1f};
		float zFar{100.0f};
		glm::mat4 proj{1.0f};
	};
}
