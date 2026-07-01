#pragma once
// detail/AYShaderProgramLegacy.h — Phase 4-E internal legacy bgfx handles
//
// NOT part of the frontend API. Used by ShaderCache draft and internal
// tooling. Frontend code must use ShaderResource instead.

#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::shader
{

struct UniformInfo {
    std::string name;
    bgfx::UniformType::Enum type;
    uint16_t binding;
};

struct TextureInfo {
    std::string name;
    uint8_t binding;
};

class ShaderProgram {
public:
    ShaderProgram() = default;

    void setVertexShader(bgfx::ShaderHandle handle) { _vertexShader = handle; }
    void setFragmentShader(bgfx::ShaderHandle handle) { _fragmentShader = handle; }

    bgfx::ShaderHandle getVertexShader() const { return _vertexShader; }
    bgfx::ShaderHandle getFragmentShader() const { return _fragmentShader; }

    void addUniform(const UniformInfo& uniform) { _uniforms.push_back(uniform); }
    void addTexture(const TextureInfo& texture) { _textures.push_back(texture); }

    const std::vector<UniformInfo>& getUniforms() const { return _uniforms; }
    const std::vector<TextureInfo>& getTextures() const { return _textures; }

    bgfx::ProgramHandle getProgramHandle() const { return _program; }
    void setProgramHandle(bgfx::ProgramHandle handle) { _program = handle; }

private:
    bgfx::ShaderHandle _vertexShader = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle _fragmentShader = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle _program = BGFX_INVALID_HANDLE;
    std::vector<UniformInfo> _uniforms;
    std::vector<TextureInfo> _textures;
};

} // namespace ayt::shader
