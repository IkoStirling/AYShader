include_guard(GLOBAL)

find_package(bgfx CONFIG REQUIRED)

set(_ays_vcpkg_bgfx_tools
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
    "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
)

# Discard the legacy in-tree prebuilt path from an existing CMake cache.
if(AY_SHADER_SHADERC_PATH MATCHES "[/\\\\]thirdParty[/\\\\]bgfx-install")
    unset(AY_SHADER_SHADERC_PATH CACHE)
    unset(AY_SHADER_SHADERC_PATH)
endif()

if(NOT AY_SHADER_SHADERC_PATH)
    find_program(AY_SHADER_SHADERC_PATH
        NAMES shaderc shaderc.exe
        HINTS ${_ays_vcpkg_bgfx_tools}
        DOC "Path to the bgfx shader compiler"
    )
endif()

if(NOT AY_SHADER_SHADERC_PATH)
    message(FATAL_ERROR
        "bgfx shaderc was not found. Install it with: "
        "vcpkg install bgfx[tools]:${VCPKG_TARGET_TRIPLET}")
endif()

# bgfxConfig.cmake exports this path. Keep a find_path fallback for other
# compatible bgfx packages.
if(AY_BGFX_SHADER_INCLUDE_DIR MATCHES "[/\\\\]thirdParty[/\\\\]bgfx-install")
    unset(AY_BGFX_SHADER_INCLUDE_DIR CACHE)
    unset(AY_BGFX_SHADER_INCLUDE_DIR)
endif()

if(NOT AY_BGFX_SHADER_INCLUDE_DIR)
    if(BGFX_SHADER_INCLUDE_PATH AND
       EXISTS "${BGFX_SHADER_INCLUDE_PATH}/bgfx_shader.sh")
        set(AY_BGFX_SHADER_INCLUDE_DIR "${BGFX_SHADER_INCLUDE_PATH}")
    else()
        get_target_property(_ays_bgfx_include_dirs
            bgfx::bgfx INTERFACE_INCLUDE_DIRECTORIES)
        find_path(AY_BGFX_SHADER_INCLUDE_DIR
            NAMES bgfx_shader.sh
            HINTS ${_ays_bgfx_include_dirs}
            PATH_SUFFIXES bgfx
        )
    endif()
endif()

if(NOT AY_BGFX_SHADER_INCLUDE_DIR)
    message(FATAL_ERROR "bgfx_shader.sh was not found in the bgfx package")
endif()

set(AY_SHADER_SHADERC_PATH "${AY_SHADER_SHADERC_PATH}" CACHE FILEPATH
    "Path to the bgfx shader compiler" FORCE)
set(AY_BGFX_SHADER_INCLUDE_DIR "${AY_BGFX_SHADER_INCLUDE_DIR}" CACHE PATH
    "Directory containing bgfx_shader.sh" FORCE)

set(_ays_tracked_bgfx_common
    "${CMAKE_CURRENT_LIST_DIR}/../shaderinclude/bgfx")

if(NOT EXISTS "${_ays_tracked_bgfx_common}/common.sh" OR
   NOT EXISTS "${_ays_tracked_bgfx_common}/shaderlib.sh")
    message(FATAL_ERROR
        "AYShader tracked bgfx shader resources are incomplete: "
        "${_ays_tracked_bgfx_common}")
endif()

# Preserve explicit external overrides, but migrate cached paths pointing at
# the old root-level bgfx source checkout.
if(NOT AY_SHADER_BGFX_COMMON_DIR OR
   AY_SHADER_BGFX_COMMON_DIR MATCHES "[/\\\\]thirdparty[/\\\\]bgfx[/\\\\]examples[/\\\\]common")
    set(AY_SHADER_BGFX_COMMON_DIR "${_ays_tracked_bgfx_common}" CACHE PATH
        "Directory containing common.sh and shaderlib.sh" FORCE)
endif()

if(NOT AY_SHADER_BGFX_SRC_DIR OR
   AY_SHADER_BGFX_SRC_DIR MATCHES "[/\\\\]thirdparty[/\\\\]bgfx[/\\\\]src")
    # Existing code calls this the bgfx source directory, but shaderc only
    # needs bgfx_shader.sh and bgfx_compute.sh from the installed include dir.
    set(AY_SHADER_BGFX_SRC_DIR "${AY_BGFX_SHADER_INCLUDE_DIR}" CACHE PATH
        "Directory containing bgfx shader include files" FORCE)
endif()

message(STATUS "bgfx shaderc: ${AY_SHADER_SHADERC_PATH}")
message(STATUS "bgfx shader includes: ${AY_BGFX_SHADER_INCLUDE_DIR}")
