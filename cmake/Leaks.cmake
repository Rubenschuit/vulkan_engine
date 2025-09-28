# Optional macOS leaks target
if(VE_USE_LEAKS AND APPLE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
  add_compile_options(-g)
  add_custom_target(leaks
    COMMAND leaks --atExit -- ./VEngine
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${PROJECT_NAME}
    COMMENT "Run macOS leaks tool on VEngine executable"
  )
endif()
