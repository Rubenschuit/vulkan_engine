#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <ve_app.hpp>

int main() {
	ve::VeApp app;
	try{
		app.run();
	} catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return 0;
}