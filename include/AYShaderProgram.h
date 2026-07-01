#pragma once
// AYShaderProgram.h - Compiled shader program (frontend-safe, no bgfx)

#include "AYCompilerError.h"

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
