# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Newer GCC (15.x) than the one in the upstream CI image flags use-after-free in
# a vendored LCD component; the code is not ours to fix and the warning is not
# fatal on the upstream toolchain.
add_compile_options(-Wno-error=use-after-free)

# The AirPlay protocol stack lives in the firmware/ tree next to this project.
# Sharing that one copy keeps the protocol code in a single place; it must be
# set before project.cmake so the components are discovered.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../firmware/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.2")
project(xiaozhi)
