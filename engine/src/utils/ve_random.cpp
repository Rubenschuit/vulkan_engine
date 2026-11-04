#include "pch.hpp"
#include "utils/ve_random.hpp"

namespace ve {

std::mt19937& Random::generator() {
	static thread_local std::mt19937 gen(std::random_device{}());
	return gen;
}

}
