#pragma once
// ShaderResourceImpl.h - internal pimpl (not installed in include/)

#include "AYShader/ShaderProgram.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::shader
{

enum class BindingKind : uint8_t {
    Uniform,
    Texture,
    UniformBlock,
    StorageBuffer,
};

struct BindingEntry {
    BindingId           id = InvalidBinding;
    BindingKind         kind = BindingKind::Uniform;
    std::string         name;
    bgfx::UniformHandle uniformHandle = BGFX_INVALID_HANDLE;
    uint8_t             textureBinding = 0;

    size_t uniformBlockSizeBytes = 0;
    uint16_t uniformSubmitCount = 1;
    // Number of source bytes consumed by one bgfx uniform element.  Pending
    // writes may intentionally provide fewer elements than the reflected
    // array capacity (for example a compact skin palette in bones[128]).
    size_t uniformElementSizeBytes = 16;
    std::unordered_map<std::string, size_t> uniformBlockFieldOffsets;
    std::unordered_map<std::string, size_t> uniformBlockFieldSizes;
};

struct PendingUniform {
    BindingId              id = InvalidBinding;
    std::vector<uint8_t>   data;
    uint16_t               submitCount = 0;
};

struct PendingTexture {
    uint8_t              stage = 0;
    BindingId              id = InvalidBinding;
    bgfx::TextureHandle  texture = BGFX_INVALID_HANDLE;
};

class ShaderResourceImpl {
public:
    bgfx::ShaderHandle  vertexShader   = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle  fragmentShader = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle  computeShader  = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle programHandle  = BGFX_INVALID_HANDLE;

    std::unordered_map<std::string, BindingId> uniformBindings;
    std::unordered_map<std::string, BindingId> textureBindings;
    std::unordered_map<std::string, BindingId> uniformBlockBindings;
    std::unordered_map<std::string, BindingId> storageBufferBindings;
    std::unordered_map<BindingId, BindingEntry> bindingsById;

    std::vector<PendingUniform> pendingUniforms;
    std::vector<PendingTexture> pendingTextures;

    void destroyGpuResources();
};

} // namespace ayt::shader
