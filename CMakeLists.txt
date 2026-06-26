cmake_minimum_required(VERSION 3.20)
project(AYShader VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# AYShader library (no ALIAS — sources and includes attach directly)
add_library(AYShader STATIC
    # Phoskia compiler core (ayt::shader::phoskia)
    src/AYLexer.cpp
    src/AYParser.cpp
    src/AYCompilerError.cpp
    src/AYType.cpp
    src/AYTypeInference.cpp
    src/AYSemanticAnalyzer.cpp
    src/AYBuiltinFunctions.cpp
    src/AYPhoskia.cpp
    # Engine integration (ayt::shader)
    src/AYBGFXConverter.cpp
    src/AYShaderProgram.cpp
    src/AYShaderCache.cpp
)

target_include_directories(AYShader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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

target_link_libraries(AYShader PUBLIC
    bgfx::bgfx
)

# Unit tests
enable_testing()
add_subdirectory(unittest)
