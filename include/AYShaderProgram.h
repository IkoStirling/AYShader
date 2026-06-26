#pragma once
// AYShaderProgram.h - Compiled shader program

#include <bgfx/bgfx.h>
#include <cstdint>
#include <vector>
#include <string>

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
    bgfx::ShaderHandle _vertexShader;
    bgfx::ShaderHandle _fragmentShader;
    bgfx::ProgramHandle _program;
    std::vector<UniformInfo> _uniforms;
    std::vector<TextureInfo> _textures;
};

} // namespace ayt::shader
