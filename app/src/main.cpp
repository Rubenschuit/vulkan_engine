#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include "ve_app.hpp"
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <vector>

// Return the path to the running executable as a filesystem::path.
// Uses a growing wchar_t buffer to handle long paths and avoids manual malloc/free.
static std::filesystem::path GetPathToRunningExe()
{
    std::vector<wchar_t> buf;
    DWORD len = 0;
    // Start with MAX_PATH and grow if truncated.
    for (size_t size = 260; ; size *= 2) {
        buf.resize(size);
        len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        // If len < buf.size() - 1 then the result was not truncated.
        if (len < buf.size() - 1) {
            return std::filesystem::path(std::wstring(buf.data(), len));
        }
        // If extremely large, bail out to avoid infinite loop.
        if (size > (1 << 20)) {
            throw std::runtime_error("Executable path too long");
        }
    }
}
#endif

int main(int argc, char** argv) {
	// argv[0] is path to executable on posix

#ifdef _WIN32
	std::filesystem::path exe_path = GetPathToRunningExe();
	exe_path = exe_path.parent_path().parent_path().parent_path(); // go up four levels to project root
#else
	std::filesystem::path exe_path = argv[0];
    exe_path = exe_path.parent_path().parent_path(); // go up two levels to project root
#endif
	

	VE_LOGI("VeApp::VeApp working_directory=" << exe_path.string());

	ve::VeApp app(exe_path);
	try {
		app.run();
	} catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	return 0;
}


