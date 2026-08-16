cmake_minimum_required(VERSION 3.20)
project(AYShader
    VERSION 1.0.0
    DESCRIPTION "AYShader — Phoskia shading language + Compiler + bgfx runtime + atan/asin builtins"
)

# C++ standard follows the root CMakeLists (currently C++23; may move to C++26).
# Do not pin a standard here.

# AYShader library (no ALIAS — sources and includes attach directly)
#
# Auto-discover sources via GLOB CONFIGURE_DEPENDS. Same rationale as
# unittest/CMakeLists.txt: a hand-maintained list works but lets CMake
# decide not to rebuild when only a header changes (or when a source
# file is renamed). For a small library like this (a dozen files)
# CONFIGURE_DEPENDS is essentially free; the re-glob cost is in the
# millisecond range on Windows.
file(GLOB AYSHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(AYShader STATIC ${AYSHADER_SOURCES})

target_include_directories(AYShader PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
    $<INSTALL_INTERFACE:include>
)

# Foundation dependencies are consumed by linking to targets added via
# add_subdirectory in the project root CMakeLists.txt (e.g. AYMath, AYTest).
# Do NOT find_package(AYFoundation) — AYFoundation is not a vcpkg-style
# installable package; it is a source tree under AYFoundation/.

# bgfx libraries, tools and shader include paths are provided by vcpkg.
# The root project includes this helper before adding Runtime modules; the
# local include keeps AYShader usable from a parent project as well.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AYBgfx.cmake")

target_link_libraries(AYShader PUBLIC
    AYIO
    bgfx::bgfx
)

if(MSVC)
    target_compile_options(AYShader PRIVATE /Zc:__cplusplus /Zc:preprocessor)
endif()

# Unit tests
enable_testing()
add_subdirectory(unittest)

message(STATUS "AYShader v1.0.0 (Phoskia P6 fragment MRT + atan/asin/acos builtins + SkyboxPass) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
