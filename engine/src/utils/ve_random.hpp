#pragma once
#include <random>
#include <glm/glm.hpp>

namespace ve {

class Random {
public:
	static void init() {
		srand(static_cast<unsigned int>(time(nullptr)));
	}

	static float float01() {
		return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
	}

	static float floatRange(float min, float max) {
		return min + (max - min) * float01();
	}

	static int intRange(int min, int max) {
		return min + static_cast<int>(rand() % (max - min + 1));
	}

	static glm::vec3 vec3Range(float min, float max) {
		return glm::vec3(floatRange(min, max), floatRange(min, max), floatRange(min, max));
	}

	static glm::vec3 vec3Range(const glm::vec3& min, const glm::vec3& max) {
		return glm::vec3(floatRange(min.x, max.x), floatRange(min.y, max.y), floatRange(min.z, max.z));
	}

	static glm::vec4 color() {
		return glm::vec4(0.3f + float01(), float01(), float01(), 1.0f);
	}
};

}

