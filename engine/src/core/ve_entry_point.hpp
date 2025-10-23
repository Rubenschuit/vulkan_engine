// This is the entry point for the application using the VEngine framework.
// It sets up the working directory and starts a Sandbox::VeApplication instance.
#include "ve_application.hpp"
#include "utils/ve_log.hpp"

#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <filesystem>
#include <string>

#if _MSC_VER && !__INTEL_COMPILER
#include <windows.h>
#include <vector>

// Return the path to the running executable as a filesystem::path.
// Uses a growing wchar_t buffer to handle long paths and avoids manual malloc/free.
static std::filesystem::path GetPathToRunningExe()
{
    std::vector<wchar_t> buf;
    DWORD len = 0;
    // Start with MAX_PATH and grow if truncated.
    const size_t MAX_PATH_SIZE = 260;
    for (size_t size = MAX_PATH_SIZE; ; size *= 2) {
        buf.resize(size);
        len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
        assert(len > 0 && "GetModuleFileNameW failed");
        // If len < buf.size() - 1 then the result was not truncated.
        if (len < buf.size() - 1) {
            return std::filesystem::path(std::wstring(buf.data(), len));
        }
        // If extremely large, bail out to avoid infinite loop.
        assert(size <= (1 << 20) && "Executable path too long");
    }
}
#endif

// Called by the entry point main() to create the application instance
extern ve::VeApplication* createApp(std::filesystem::path working_directory);

int main(int argc, char** argv) {
	(void)argc; // unused

#if _MSC_VER && !__INTEL_COMPILER
	std::filesystem::path path = GetPathToRunningExe();
#else
	// argv[0] is path to executable on posix
	std::filesystem::path path = argv[0];
#endif
	// reduce something like root/build/Debug/VeApp to root
	path = path.parent_path().parent_path().parent_path(); // executable is in build/Debug|Release/

	// Simple checks for expected subfolders
	assert(std::filesystem::exists(path) && "Working directory does not exist");
	assert(std::filesystem::exists(path / "models") && "Working directory 'models' subfolder does not exist");
	assert(std::filesystem::exists(path / "textures") && "Working directory 'textures' subfolder does not exist");

	VE_LOGD("VeApp::VeApp working_directory=" << path.string());

	auto app = createApp(path);
	app->run();
	delete app;
}