#pragma once
#include <random>
#include <glm/glm.hpp>

namespace ve {

class Random {
public:
	static float float01() {
        static thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(generator());
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
		return glm::vec4(float01(), float01(), float01(), 1.0f);
	}

	static glm::vec4 colorHSV() {
		float h = float01(); // Hue: 0-1
		float s = floatRange(0.6f, 1.0f); // Saturation: 60-100% (vibrant)
		float v = floatRange(0.8f, 1.0f); // Value: 80-100% (bright)

		// Convert HSV to RGB
		float c = v * s;
		float x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
		float m = v - c;

		float r, g, b;
		if (h < 1.0f/6.0f) { r = c; g = x; b = 0; }
		else if (h < 2.0f/6.0f) { r = x; g = c; b = 0; }
		else if (h < 3.0f/6.0f) { r = 0; g = c; b = x; }
		else if (h < 4.0f/6.0f) { r = 0; g = x; b = c; }
		else if (h < 5.0f/6.0f) { r = x; g = 0; b = c; }
		else { r = c; g = 0; b = x; }

		return glm::vec4(r + m, g + m, b + m, 1.0f);
	}

private:
	static std::mt19937& generator() {
		static thread_local std::mt19937 gen(std::random_device{}());
		return gen;
}
};

}

