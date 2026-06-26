#pragma once
// IAYBackendConverter.h - Backend converter interface for Phoskia
//
// A backend converter takes a Phoskia AST and produces shader source code
// for a specific target platform (BGFX .sc, HLSL, GLSL, WGSL, ...).
//
// Phase 1: only AYBGFXConverter is implemented. The interface is designed
// to allow additional backends without changes to the Phoskia core.

#include "AYAst.h"
#include <string>
#include <vector>
#include <string_view>

namespace ayt::shader
{

enum class Platform : uint8_t {
    DX11,
    DX12,
    OpenGL,
    Vulkan,
    WebGPU,
    BGFX
};

// Generic uniform / texture binding info populated by a backend.
struct BackendUniformInfo {
    std::string name;
    std::string type;
    BackendUniformInfo() = default;
    BackendUniformInfo(std::string n, std::string t) : name(std::move(n)), type(std::move(t)) {}
};

struct BackendTextureInfo {
    std::string name;
    uint8_t binding = 0;
    BackendTextureInfo() = default;
    BackendTextureInfo(std::string n, uint8_t b) : name(std::move(n)), binding(b) {}
};

struct ConvertResult {
    bool success = false;
    std::string output;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    // Backend-specific binding info. Empty for backends that don't produce this.
    std::vector<BackendUniformInfo> uniforms;
    std::vector<BackendTextureInfo> textures;
};

class IAYBackendConverter {
public:
    virtual ~IAYBackendConverter() = default;

    // Which platform this converter targets.
    virtual Platform targetPlatform() const = 0;

    // File extension for the intermediate source (e.g. ".sc" for BGFX).
    virtual const char* targetExtension() const = 0;

    // Convert Phoskia AST to target shader source.
    virtual ConvertResult convert(const phoskia::Program& ast) = 0;

    // Compiler arguments for the platform's shaderc invocation.
    virtual std::vector<std::string_view> getCompilerArgs() const = 0;
};

} // namespace ayt::shader
