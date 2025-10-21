#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include "ve_app.hpp"
#include <filesystem>
#include <string>

// Windows-specific function to get the path to the running executable
#ifdef _WIN32
#include <windows.h>
LPCWSTR GetPathToRunningExe()
{
    wchar_t szPath[MAX_PATH] = { 0 };
    DWORD dwSize = GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath));
    wchar_t* result = NULL;

    if (dwSize > 0)
    {
        size_t count = wcslen(szPath) + 1;
        result = (wchar_t*)malloc(count * sizeof(szPath[0]));
        wcscpy_s(result, count, szPath);
    }
    return result;
}
#endif

int main(int argc, char** argv) {
	// argv[0] is path to executable on posix

#ifdef _WIN32
	LPCWSTR exe_path_w = GetPathToRunningExe();
	std::wstring exe_path_ws(exe_path_w);
	std::string path(exe_path_ws.begin(), exe_path_ws.end());
	free((void*)exe_path_w);
	std::filesystem::path exe_path(exe_path);
#else
	std::filesystem::path exe_path = argv[0];
#endif
	exe_path = exe_path.parent_path().parent_path(); // go up two levels to project root

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


