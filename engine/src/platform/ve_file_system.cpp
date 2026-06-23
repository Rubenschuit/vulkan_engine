#include "pch.hpp"
#include "platform/ve_file_system.hpp"
#include "utils/ve_log.hpp"
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

namespace ve {

#if !defined(_WIN32)
static char** currentEnviron() {
#if defined(__APPLE__)
	return *_NSGetEnviron();
#else
	extern char** environ;
	return environ;
#endif
}

// Launch a program with an explicit argv.
// Used to reveal a file in the OS file manager.
static void launchProcess(const std::vector<std::string>& args) {
	std::vector<char*> argv;
	argv.reserve(args.size() + 1);
	for (const std::string& a : args)
		argv.push_back(const_cast<char*>(a.c_str()));
	argv.push_back(nullptr);

	pid_t pid = 0;
	int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), currentEnviron());
	if (rc != 0) {
		VE_LOGE("revealInFileManager: failed to launch '" << args[0] << "': " << std::strerror(rc));
		return;
	}
	int status = 0;
	waitpid(pid, &status, 0);
}
#endif

// Reads a binary file and returns its contents as a vector of chars
std::vector<char> VeFileSystem::readFile(const std::filesystem::path& file_path) {

	std::ifstream file(file_path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		throw std::runtime_error(std::string("failed to open file: ") + file_path.string());
	}
	//VE_LOGI("Reading file: " << file_path.string());

	std::streampos end = file.tellg();
	file.seekg(0, std::ios::beg);
	size_t file_size = static_cast<size_t>(end - file.tellg());
	std::vector<char> buffer(file_size);

	file.seekg(0);
	file.read(buffer.data(), static_cast<std::streamsize>(file_size));
	file.close();
	return buffer;
}

void VeFileSystem::revealInFileManager(const std::filesystem::path& path) {
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		VE_LOGE("revealInFileManager: path does not exist: " << path.string());
		return;
	}

#if defined(__APPLE__)
	// -R selects the item in Finder
	launchProcess({"open", "-R", path.string()});
#elif defined(_WIN32)
	std::wstring native = std::filesystem::path(path).make_preferred().wstring();
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (std::filesystem::is_directory(path, ec)) {
		ShellExecuteW(nullptr, L"open", native.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	} else if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(native.c_str())) {
		SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
		ILFree(pidl);
	}
	if (SUCCEEDED(hr))
		CoUninitialize();
#else
	// No "select file" on Linux
	std::filesystem::path dir = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
	launchProcess({"xdg-open", dir.string()});
#endif
}

}
