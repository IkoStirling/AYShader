#pragma once
// AYShader/ShaderProgram.h - Compiled shader program (frontend-safe, no bgfx)

#include "AYShader/CompilerError.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ayt::shader
{

struct BGFXUniform {
    std::string name;
    std::string type;
    uint8_t count = 1;
    std::string blockName;
    int blockBinding = -1;
};

struct BGFXTexture {
    std::string name;
    uint8_t binding = 0;
    std::string textureType = "sampler2D";
};

namespace detail
{

// On D3D, bgfx's SAMPLER* macros append "Sampler" and "Texture" to the
// declared symbol. shaderc removes those markers again while building its
// reflection table, starting at their first occurrence rather than requiring
// them to be a suffix. A logical name such as "baseColorTexture" can therefore
// be reflected as "baseColor" and collide with an unrelated uniform. Keep the
// public/material-facing name unchanged and use a stable backend-only hash
// spelling which cannot itself contain either marker.
inline std::string bgfxTextureSymbolName(const std::string& logicalName)
{
    if (logicalName.find("Texture") == std::string::npos
        && logicalName.find("Sampler") == std::string::npos) {
        return logicalName;
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char ch : logicalName) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= UINT64_C(1099511628211);
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string backendName = "ayt_t_";
    backendName.resize(6 + 16);
    for (size_t i = 0; i < 16; ++i) {
        const size_t shift = (15 - i) * 4;
        backendName[6 + i] = kHex[(hash >> shift) & UINT64_C(0xf)];
    }
    return backendName;
}

} // namespace detail

struct BGFXUniformBlockMember {
    std::string name;
    std::string type;
    size_t      offsetBytes = 0;
    size_t      sizeBytes = 0;
};

struct BGFXUniformBlock {
    std::string name;
    int binding = -1;
    size_t sizeBytes = 0;
    std::vector<std::string> fieldNames;
    std::vector<BGFXUniformBlockMember> members;
};

struct BGFXStorageBuffer {
    std::string name;
    int binding = -1;
    std::string elementType;
};

struct CompiledShaderProgram {
    bool                       success = false;
    std::vector<std::string>   errors;
    std::vector<std::string>   warnings;
    std::vector<phoskia::PhoskiaDiagnostic> diagnostics;
    std::vector<phoskia::PhoskiaDiagnostic> diagnosticWarnings;

    std::vector<uint8_t>       vsBin;
    std::vector<uint8_t>       fsBin;
    std::vector<uint8_t>       csBin;

    std::vector<BGFXUniformBlock>    uniformBlocks;
    std::vector<BGFXStorageBuffer>   storageBuffers;
    std::vector<BGFXUniform>         uniforms;
    std::vector<BGFXTexture>         textures;

    std::map<std::string, std::string>   sources;
};

} // namespace ayt::shader
