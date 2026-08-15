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

# bgfx is the only third-party dependency. The library is vendored under
# thirdParty/bgfx-install/ (copied from AliyatRenderer). The find_package
# call below relies on bgfxConfig.cmake being located at
# thirdParty/bgfx-install/debug/lib/cmake/bgfx/. The config computes
# PACKAGE_PREFIX_DIR via `${CMAKE_CURRENT_LIST_DIR}/../../../` which
# resolves to thirdParty/bgfx-install/.
list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdParty/bgfx-install/debug"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdParty/bgfx-install/release"
)
find_package(bgfx CONFIG REQUIRED)

# fix-bgfx-imported-targets (PR-5 LM-2 unblock):
# Vendored thirdParty/bgfx-install/ only ships per-config CMake files for
# Debug and Release (bgfxTargets-debug.cmake / bgfxTargets-release.cmake);
# there is no RelWithDebInfo variant. Without the explicit
# IMPORTED_LOCATION_* below, any consumer built with
# CMAKE_BUILD_TYPE=RelWithDebInfo (or via VS multi-config where
# MAP_IMPORTED_CONFIG_RELWITHDEBINFO defaults to Release, but Release's
# _NDEBUG bgfx.lib collides with _DEBUG consumers built from the standard
# `out/build/x64-Debug` build dir) fails to link with
# "imported target 'bgfx::bgfx' has no IMPORTED_LOCATION for configuration
# 'RelWithDebInfo'" / LNK1104.
#
# Pure additive — for Debug (CMAKE_BUILD_TYPE=Debug, the standard x64-Debug
# build dir) and Release the populated paths are identical to what
# bgfxTargets-{debug,release}.cmake set; we re-assert them explicitly so
# the property is non-empty for every config CMake might evaluate.
# RelWithDebInfo resolves to the debug tree (BX_CONFIG_DEBUG=1) because
# the standard build dir compiles consumers with _DEBUG.
set(_bgfx_libdir_debug   "${CMAKE_CURRENT_SOURCE_DIR}/thirdParty/bgfx-install/debug/lib")
set(_bgfx_libdir_release "${CMAKE_CURRENT_SOURCE_DIR}/thirdParty/bgfx-install/release/lib")

foreach(_lib IN ITEMS bgfx bx bimg bimg_decode bimg_encode)
    set_property(TARGET bgfx::${_lib} APPEND PROPERTY
        IMPORTED_CONFIGURATIONS DEBUG RELEASE RELWITHDEBINFO)
    set_target_properties(bgfx::${_lib} PROPERTIES
        IMPORTED_LOCATION_DEBUG          "${_bgfx_libdir_debug}/${_lib}.lib"
        IMPORTED_LOCATION_RELEASE        "${_bgfx_libdir_release}/${_lib}.lib"
        IMPORTED_LOCATION_RELWITHDEBINFO "${_bgfx_libdir_debug}/${_lib}.lib"
    )
endforeach()

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
