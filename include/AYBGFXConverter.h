#pragma once
// AYBGFXConverter.h - BGFX backend converter (Phoskia → vs/fs/varying.def.sc)
//
// Lives in ayt::shader (engine integration) because it is the only real
// backend in Phase 1. Phoskia itself (Token/Lexer/Parser/AST) lives in
// ayt::shader::phoskia.

#include "IAYBackendConverter.h"
#include "AYAst.h"
#include "AYIr.h"
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

// One compute declaration's output: the single .sc source that bgfx's
// shaderc compiles with `--type compute`. Compute has no vs/fs/varying
// split and no varying.def.sc (it has no attributes, no varyings).
//
// Phase 3.2: the compiler body is empty-only — no storage buffers, no
// thread-id builtins. The two additions land in the next two blocks
// (storage buffer syntax + thread-id builtins).
struct BGFXComputeFile {
    std::string cs;           // cs_<Compute>.sc contents
};

// One material → three artifact strings. The frontend (upper layer) is
// responsible for writing these to disk and invoking shaderc.
struct BGFXUniform {
    std::string name;
    std::string type;
    uint8_t count = 1;
    // Phase 3.4: see BackendUniformInfo::blockName / blockBinding
    // for the field-vs-block distinction. `blockName == ""` is a
    // legacy `uniform T x;` decl; otherwise it's a UBO field.
    std::string blockName;
    int blockBinding = -1;
};

struct BGFXTexture {
    std::string name;
    uint8_t binding = 0;
    std::string textureType = "sampler2D";
};

// Phase 3.4: uniform buffer object (GLSL `uniform Name { ... } name;`).
// `sizeBytes` is the std140 block size, computed by the GLSL
// compiler (captured at runtime via `bgfx::getUniformBlockSize` or
// similar — Phase 3.4 leaves it 0; host populates after the shader
// is compiled). `fieldNames` mirrors the AST field order so the
// host can compute per-field std140 offsets if it needs them.
struct BGFXUniformBlock {
    std::string name;
    int binding = -1;
    size_t sizeBytes = 0;
    std::vector<std::string> fieldNames;
};

// Phase 3.5-A: storage buffer (GLSL `buffer Name { T data[]; } Name;`).
// Distinct from BGFXUniformBlock because storage and uniform buffers
// live in different descriptor sets in the underlying graphics API
// (DX11: t# vs b# registers; Vulkan: STORAGE vs UNORM descriptor
// types). The host code that uploads them and binds them at draw
// time is different too, so we keep a separate parallel struct.
// `binding` is the final GLSL `binding = N` slot — either the
// user-written literal or the auto-assigned slot (the BGFX backend
// resolves -1 → auto at emit time).
struct BGFXStorageBuffer {
    std::string name;
    int binding = -1;
    std::string elementType;
};

struct BGFXConvertResult {
    bool success = false;
    // Per-material three-piece sets, in declaration order. Empty when
    // success == false.
    std::vector<BGFXShaderFiles> materialFiles;
    // Per-compute single-piece sets, in declaration order. Phase 3.2:
    // BGFX .sc IS the compute target backend (bgfx 1.18 / shaderc 1.18
    // both support `--type compute` and `bgfx::createProgram(_csh)`),
    // so compute declarations are first-class here just like materials.
    std::vector<BGFXComputeFile> computeFiles;
    std::vector<BGFXUniform> uniforms;
    std::vector<BGFXTexture> textures;
    // Phase 3.4: top-level UBO decls (one per UniformBlockDecl).
    // The frontend can iterate these to compute per-block std140
    // sizes via shaderc and to wire up `bgfx::setUniform(handle, ptr,
    // sizeof(block))` calls at draw time.
    std::vector<BGFXUniformBlock> uniformBlocks;
    // Phase 3.5-A: storage buffer decls (one per compute storage
    // decl). The frontend wires these up via the bgfx compute
    // path (`bgfx::setUniform(handle, ptr, sizeof(buffer))` at
    // dispatch time). The host reads `binding` to know which
    // GLSL binding slot to target.
    std::vector<BGFXStorageBuffer> storageBuffers;
    std::vector<std::string> errors;
};

// BGFX backend converter (Phoskia → BGFX .sc three-piece set)
class AYBGFXConverter : public IAYBackendConverter {
public:
    Platform targetPlatform() const override { return Platform::BGFX; }
    const char* targetExtension() const override { return ".sc"; }

    BGFXConvertResult convertBGFX(const phoskia::ir::IRProgram& program);
    // Out-param overload — preferred entry point. The return-by-value
    // overload above remains for callers that can tolerate the SSO /
    // NRVO risk; production paths in this repo use the out-param form
    // (the Phase 3.2-pre SSO bug fix landed the same pattern for
    // Compiler::compile / CompileResult).
    void convertBGFX(const phoskia::ir::IRProgram& program, BGFXConvertResult& out);
    ConvertResult convert(const phoskia::ir::IRProgram& program) override;

    // Compile one material into its three-piece set.
    BGFXShaderFiles convertMaterial(const phoskia::ir::IRMaterialDecl& material);

    // Compile one compute declaration into its single-piece .sc output.
    // Phase 3.2: BGFX .sc is the compute target backend — bgfx 1.18 +
    // shaderc 1.18 fully support compute via `bgfx::createProgram(_csh)`
    // and `shaderc --type compute`. Earlier Phase 2.5 had a placeholder
    // that claimed BGFX .sc does not support compute — that conclusion
    // was wrong and has been corrected. Compute declarations are now
    // first-class alongside materials.
    BGFXComputeFile convertComputeDecl(const phoskia::ir::IRComputeDecl& compute);

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
    void generateProperty(const phoskia::ir::IRDeclaration& decl);
    void generateExpr(const phoskia::ir::IRExpr& expr);

    // Temporary state used during convertMaterial(); populated by the
    // helper methods above and consumed when assembling the three files.
    std::string _uniformDecls;
    std::string _textureDecls;
    std::string _propertyUniforms;
    std::vector<BGFXUniform> _uniforms;
    std::vector<BGFXTexture> _textures;
    // Phase 3.4: pre-emitted UBO decls (one `layout(std140, binding = N)
    // uniform Name { ... } Name;` per UniformBlockDecl). The
    // convertBGFX top-level loop fills this once, then convertMaterial
    // / convertComputeDecl splice the same string into every shader
    // stage's output (vs + fs for materials; cs for compute). UBO
    // decls are global — every stage that uses a block needs the
    // decl visible, and GLSL allows the same block in multiple
    // stages (the GLSL compiler dedupes per compilation unit).
    std::string _uboDecls;
    // Same data, structured for the frontend. Populated alongside
    // `_uboDecls`; carried across convertMaterial / convertComputeDecl
    // to flush into the final BGFXConvertResult.
    std::vector<BGFXUniformBlock> _uniformBlocks;
    // Phase 3.5-A: storage buffer binding info collected during
    // convertComputeDecl. Cleared at the top of convertBGFX (one
    // entry per compute storage decl, with binding resolved
    // including any auto-assigned slots).
    std::vector<BGFXStorageBuffer> _storageBuffers;
};

} // namespace ayt::shader
