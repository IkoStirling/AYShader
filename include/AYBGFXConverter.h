#pragma once
// AYBGFXConverter.h - BGFX backend converter (Phoskia → .sc format)
//
// Lives in ayt::shader (engine integration) because it is the only real
// backend in Phase 1. Phoskia itself (Token/Lexer/Parser/AST) lives in
// ayt::shader::phoskia.

#include "IAYBackendConverter.h"
#include "AYAst.h"
#include <string>
#include <vector>

namespace ayt::shader
{

// BGFX shader types
enum class BGFXShaderType {
    Vertex,
    Fragment,
    Compute,
    Ray
};

// BGFX uniform info
struct BGFXUniform {
    std::string name;
    std::string type;
    uint8_t count = 1;
};

// BGFX texture info
struct BGFXTexture {
    std::string name;
    uint8_t binding = 0;
    std::string textureType = "sampler2D";
};

// Convert result for BGFX
struct BGFXConvertResult {
    bool success = false;
    std::string output;
    std::vector<BGFXUniform> uniforms;
    std::vector<BGFXTexture> textures;
    std::vector<std::string> errors;
    BGFXShaderType shaderType = BGFXShaderType::Fragment;
};

// BGFX backend converter (Phoskia → .sc)
class AYBGFXConverter : public IAYBackendConverter {
public:
    Platform targetPlatform() const override { return Platform::BGFX; }
    const char* targetExtension() const override { return ".sc"; }

    BGFXConvertResult convertBGFX(const phoskia::Program& ast);
    ConvertResult convert(const phoskia::Program& ast) override;

    // Generate .sc file content directly
    std::string generateSC(const phoskia::Program& ast);

    // Set shader type (vertex/fragment)
    void setShaderType(BGFXShaderType type) { _shaderType = type; }
    BGFXShaderType getShaderType() const { return _shaderType; }

    std::vector<std::string_view> getCompilerArgs() const override {
        switch (_shaderType) {
            case BGFXShaderType::Vertex: return { "-p", "vulkan", "--type", "vertex" };
            case BGFXShaderType::Fragment: return { "-p", "vulkan", "--type", "fragment" };
            case BGFXShaderType::Compute: return { "-p", "vulkan", "--type", "compute" };
            default: return { "-p", "vulkan" };
        }
    }

    // Access collected uniform / texture info (for runtime binding)
    const std::vector<BGFXUniform>& getUniforms() const { return _uniforms; }
    const std::vector<BGFXTexture>& getTextures() const { return _textures; }

private:
    void generateShaderBlock(const phoskia::MaterialDecl& material, bool isVertex);
    void generateProperty(const phoskia::PropertyDecl& prop);
    void generateUniform(const phoskia::UniformDecl& uniform);
    void generateTexture(const phoskia::TextureDecl& texture);
    void generateShading(const phoskia::ShadingFunc& shading);
    void generateExpr(const phoskia::Expr& expr);

    BGFXShaderType _shaderType = BGFXShaderType::Fragment;
    std::string _output;
    std::vector<BGFXUniform> _uniforms;
    std::vector<BGFXTexture> _textures;
};

} // namespace ayt::shader
