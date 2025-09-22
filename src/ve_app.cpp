#include "ve_app.hpp"

namespace ve {
    VeApp::VeApp() {
    }

    VeApp::~VeApp() {
    }

    void VeApp::run() {
        while (!glfwWindowShouldClose(window.getGLFWwindow())) {
            glfwPollEvents();
        }
    }
}
