// This is the entry point for the application using the VEngine framework.
// It sets up the project root directory and starts a Sandbox::VeApplication instance.
#include "application/ve_application.hpp"
#include "utils/ve_path.hpp"
#include "utils/ve_log.hpp"

#include <filesystem>


// Called by the entry point main() to create the application instance
extern ve::VeApplication* createApp(std::filesystem::path project_root);

// Application entry point
int main(int argc, char** argv) {
	(void)argc; // unused
	VE_LOGI("Starting application");

	std::filesystem::path project_root;
	try {
		project_root = ve::getProjectRoot(argv);
		VE_LOGD("Found project root: " << project_root);
	}
	catch (const std::exception& e) {
		VE_LOGE("Failed to initialize: " << e.what());
		return 1;
	}

	auto* app = createApp(project_root);
	app->run();
	delete app;

	return 0;
}