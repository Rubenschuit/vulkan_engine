// This is the entry point for the application using the VEngine framework.
// It sets up the working directory and starts a Sandbox::VeApplication instance.
#include "ve_application.hpp"
#include "utils/ve_path.hpp"

#include <filesystem>


// Called by the entry point main() to create the application instance
extern ve::VeApplication* createApp(std::filesystem::path working_directory);

// Application entry point
int main(int argc, char** argv) {
	(void)argc; // unused

	std::filesystem::path working_directory = ve::getWorkingDirectory(argv);
	auto* app = createApp(working_directory);
	app->run();
	delete app;
}