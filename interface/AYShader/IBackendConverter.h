#pragma once
// AYShader/IBackendConverter.h - Backend converter interface for Phoskia
//
// A backend converter takes a Phoskia AST and produces shader source code
// for a specific target platform (BGFX .sc, HLSL, GLSL, WGSL, ...).
//
// Phase 1: only AYBGFXConverter is implemented. The interface is designed
// to allow additional backends without changes to the Phoskia core.

#include "AYShader/Ast.h"
#include "AYShader/Ir.h"
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
    // Phase 3.4: if this uniform is a field of a UBO, `blockName`
    // and `blockBinding` identify the parent block. Empty string /
    // -1 when the uniform is a plain `uniform T x;` (legacy shape).
    // Server-side std140 offsets are deferred to Phase 5+ — HLSL
    // cbuffer packoffset is the only place we'd need them.
    std::string blockName;
    int blockBinding = -1;
};

struct BackendTextureInfo {
    std::string name;
    uint8_t binding = 0;
    BackendTextureInfo() = default;
    BackendTextureInfo(std::string n, uint8_t b) : name(std::move(n)), binding(b) {}
};

struct ConvertResult {
    bool success = false;
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

    // Convert a Phoskia IR (post-Phase 3.1) to target shader source.
    // Phase 3.1+ backends consume the IR — see AYShader/Ir.h. The IR carries
    // pre-resolved types so backends do not re-run TypeInference.
    virtual ConvertResult convert(const phoskia::ir::IRProgram& program) = 0;

    // Compiler arguments for the platform's shaderc invocation.
    virtual std::vector<std::string_view> getCompilerArgs() const = 0;
};

} // namespace ayt::shader
