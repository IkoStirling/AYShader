#pragma once
// AYBGFXConverter.h - BGFX backend converter (Phoskia → vs/fs/varying.def.sc)
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

// BGFX shader types — kept for compile-time dispatch. Phase 1 only emits
// Vertex / Fragment. Compute/Ray are TODO(phase2-compute) / phase3.
enum class BGFXShaderType {
    Vertex,
    Fragment,
    Compute,
    Ray
};

// Three-piece output for a single material: the two shader programs plus
// the shared varying.def.sc that bgfx shaderc consumes.
struct BGFXShaderFiles {
    std::string vs;           // vs_<Material>.sc contents
    std::string fs;           // fs_<Material>.sc contents
    std::string varyingDef;   // varying.def.sc contents
};

// One material → three artifact strings. The frontend (upper layer) is
// responsible for writing these to disk and invoking shaderc.
struct BGFXUniform {
    std::string name;
    std::string type;
    uint8_t count = 1;
};

struct BGFXTexture {
    std::string name;
    uint8_t binding = 0;
    std::string textureType = "sampler2D";
};

struct BGFXConvertResult {
    bool success = false;
    // Per-material three-piece sets, in declaration order. Empty when
    // success == false.
    std::vector<BGFXShaderFiles> materialFiles;
    std::vector<BGFXUniform> uniforms;
    std::vector<BGFXTexture> textures;
    std::vector<std::string> errors;
};

// BGFX backend converter (Phoskia → BGFX .sc three-piece set)
class AYBGFXConverter : public IAYBackendConverter {
public:
    Platform targetPlatform() const override { return Platform::BGFX; }
    const char* targetExtension() const override { return ".sc"; }

    BGFXConvertResult convertBGFX(const phoskia::Program& ast);
    ConvertResult convert(const phoskia::Program& ast) override;

    // Compile one material into its three-piece set.
    BGFXShaderFiles convertMaterial(const phoskia::MaterialDecl& material);

    std::vector<std::string_view> getCompilerArgs() const override {
        // BGFX backend emits a three-piece set per material; the frontend
        // chooses per-target shaderc flags. Returning an empty list here
        // keeps the IAYBackendConverter contract honest without committing
        // to a specific profile.
        return {};
    }

    const std::vector<BGFXUniform>& getUniforms() const { return _uniforms; }
    const std::vector<BGFXTexture>& getTextures() const { return _textures; }

private:
    void generateProperty(const phoskia::PropertyDecl& prop);
    void generateExpr(const phoskia::Expr& expr);

    // Temporary state used during convertMaterial(); populated by the
    // helper methods above and consumed when assembling the three files.
    std::string _uniformDecls;
    std::string _textureDecls;
    std::string _propertyUniforms;
    std::vector<BGFXUniform> _uniforms;
    std::vector<BGFXTexture> _textures;
};

} // namespace ayt::shader
