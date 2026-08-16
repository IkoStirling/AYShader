# AYShader unit tests

set(TEST_NAME AYShader_Test)

# Auto-discover test source files. AYTest registers every TEST_CASE
# via a global static initialiser (_reg_##name = registerTest(...)),
# so each new TEST_CASE added to an existing .cpp is a brand-new
# global symbol that the linker must pull in. With a hand-maintained
# list, CMake doesn't re-link AYShader_Test.exe when an existing .cpp
# grows new TEST_CASE blocks (the .cpp mtime updates, but the
# incremental linker heuristic can decide the .exe is "fresh enough"
# and skip �?a common Ninja / VS generator quirk).
#
# CONFIGURE_DEPENDS asks CMake to re-glob on every build: a new
# .cpp here triggers a full reconfigure (cheap on a project this
# size) and guarantees the next link picks up the new symbols.
file(GLOB TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${TEST_NAME} ${TEST_FILES})

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(${TEST_NAME} PRIVATE
    AYShader
    AYTest
)

# Golden-file fixtures: source .phoskia + expected .sc baselines.
# The directory is copied verbatim into the build tree next to the
# test binary so a relative `unittest/golden` lookup resolves both
# during in-source builds and after `cmake --install`.
#
# Defined AFTER add_executable so add_dependencies can target the
# real ${TEST_NAME} executable (CMake rejects target-level deps on
# non-existent targets).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/golden/.stamp
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/golden
        ${CMAKE_CURRENT_BINARY_DIR}/golden
    COMMAND ${CMAKE_COMMAND} -E touch
        ${CMAKE_CURRENT_BINARY_DIR}/golden/.stamp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/golden
    COMMENT "Syncing unittest/golden/ into the build tree"
    VERBATIM
)
add_custom_target(ay_shader_golden_fixtures ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/golden/.stamp
)
add_dependencies(${TEST_NAME} ay_shader_golden_fixtures)

# AYBgfx.cmake provides shaderc from vcpkg, tracked common shader resources
# from AYShader, and bgfx_shader.sh/bgfx_compute.sh from the package include.
target_compile_definitions(${TEST_NAME} PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

if(MSVC)
    target_compile_options(${TEST_NAME} PRIVATE /Zc:__cplusplus /Zc:preprocessor)
endif()

ay_add_test(${TEST_NAME})
