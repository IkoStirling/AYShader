# AYShader unit tests

set(TEST_NAME AYShader_Test)

set(TEST_FILES
    main.cpp
    Test_Lexer.cpp
    Test_Parser.cpp
    Test_Phoskia.cpp
    Test_BGFXConverter.cpp
    Test_ShaderCompile.cpp
    Test_TypeInference.cpp
    Test_SemanticAnalyzer.cpp
    Test_PBRFunctions.cpp
    Test_ParserRecovery.cpp
    Test_BuiltinTypes.cpp
)

add_executable(${TEST_NAME} ${TEST_FILES})

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(${TEST_NAME} PRIVATE
    AYShader
    AYTest
)

# Inject the absolute path to the bgfx shaderc binary so the
# end-to-end test can locate it regardless of cwd. The default
# install layout lives under thirdParty/bgfx-install/<config>/bin/.
# Override with -DAY_SHADER_SHADERC_PATH=/full/path/to/shaderc.exe.
if(NOT DEFINED AY_SHADER_SHADERC_PATH)
    if(WIN32)
        set(_ays_shaderc_name "shaderc.exe")
    else()
        set(_ays_shaderc_name "shaderc")
    endif()
    set(AY_SHADER_SHADERC_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdParty/bgfx-install/debug/bin/${_ays_shaderc_name}")
endif()
# shaderc needs `-i` paths to find bgfx's `common.sh` (which itself
# `#include`s `bgfx_shader.sh`). These live in the bgfx source tree,
# not the bgfx-install/ build output. Override with
# -DAY_SHADER_BGFX_COMMON_DIR=/path and -DAY_SHADER_BGFX_SRC_DIR=/path
# when running outside this repo layout.
if(NOT DEFINED AY_SHADER_BGFX_COMMON_DIR)
    # Walk up from AYShader to find the bgfx source tree. The user
    # typically has bgfx checked out as a sibling project (D:/Projects/
    # thirdparty/bgfx). Fall back to the most common sibling location.
    set(_ays_bgfx_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/bgfx/examples/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../thirdparty/bgfx/examples/common"
    )
    set(AY_SHADER_BGFX_COMMON_DIR "")
    foreach(_dir IN LISTS _ays_bgfx_search)
        if(EXISTS "${_dir}/common.sh")
            set(AY_SHADER_BGFX_COMMON_DIR "${_dir}")
            break()
        endif()
    endforeach()
    if(NOT AY_SHADER_BGFX_COMMON_DIR)
        message(WARNING "AYShader end-to-end shaderc test needs bgfx's common.sh. "
            "Set -DAY_SHADER_BGFX_COMMON_DIR=/path/to/bgfx/examples/common and "
            "-DAY_SHADER_BGFX_SRC_DIR=/path/to/bgfx/src to enable the test.")
    endif()
endif()
if(NOT DEFINED AY_SHADER_BGFX_SRC_DIR AND AY_SHADER_BGFX_COMMON_DIR)
    set(AY_SHADER_BGFX_SRC_DIR "${AY_SHADER_BGFX_COMMON_DIR}/../../src")
endif()
target_compile_definitions(${TEST_NAME} PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
