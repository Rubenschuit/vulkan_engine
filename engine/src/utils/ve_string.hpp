#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace ve {

inline std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}
inline bool iequalsChar(char a, char b) {
	return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}

}