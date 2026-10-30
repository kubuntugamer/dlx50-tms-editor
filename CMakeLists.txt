cmake_minimum_required(VERSION 3.16)
project(tms_editor LANGUAGES NONE)

# Compatibility wrapper for developers who still invoke CMake. The app itself
# is Rust and does not discover, link, or ship Qt.
find_program(CARGO_EXECUTABLE cargo REQUIRED)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CARGO_PROFILE --release)
else()
    set(CARGO_PROFILE)
endif()

add_custom_target(tms_editor ALL
    COMMAND ${CARGO_EXECUTABLE} build ${CARGO_PROFILE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Building the Rust TMS editor")

add_custom_target(check
    COMMAND ${CARGO_EXECUTABLE} test --all-targets
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL)
