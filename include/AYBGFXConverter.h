#pragma once
// AYBGFXConverter.h - BGFX backend converter (Phoskia → vs/fs/varying.def.sc)
//
// Lives in ayt::shader (engine integration) because it is the only real
// backend in Phase 1. Phoskia itself (Token/Lexer/Parser/AST) lives in
// ayt::shader::phoskia.

#include "IAYBackendConverter.h"
#include "AYAst.h"
#include "AYIr.h"
#include "AYShaderProgram.h"
#include "detail/AYBGFXStageSources.h"
#include "AYShadercDriver.h"  // Phase 3.6: _driver is a unique_ptr<AYShadercDriver>;
                                // MSVC <memory>'s unique_ptr destructor requires the
                                // complete type at the point of destruction (i.e. every
                                // TU that includes this header transitively). The driver
                                // header is small and pulls in only stdint / std::string /
                                // std::vector, so the transitive cost is negligible.
#include <memory>
#include <string>
#include <vector>

namespace ayt::shader
{

// AYShadercDriver is included above (see the include rationale comment).

// BGFX shader types — kept for compile-time dispatch. Phase 1 only emits
// Vertex / Fragment. Compute/Ray are TODO(phase2-compute) / phase3.
enum class BGFXShaderType {
    Vertex,
    Fragment,
    Compute,
    Ray
};

// Compile options for AYBGFXConverter::compileToBinary(). Lives at the
// converter level (not on the Compiler) so the BGFX backend doesn't
// depend on the Compiler. Commit 3 of Phase 3.6 bridges
// `phoskia::CompileOptions` to this struct when building a
// `CompiledShaderProgram` from the frontend entry point.
struct BGFXCompileOptions {
    // Per-call shaderc executable path override. Empty by default —
    // when empty, `compileToBinary` falls back to the process-wide
    // default configured via `AYShadercDriver::setDefaultExecutable`.
    // Non-empty here means "use THIS shaderc for this compile, not
    // the global default" (useful for tests; rare in production).
    std::string              shadercPath;

    // shaderc invocation parameters.
    std::string              platform = "linux";   // bgfx shaderc --platform value
    std::string              profile  = "430";     // GLSL profile (-p value)
    std::vector<std::string> includeDirs;          // bgfx common.sh + src
    std::vector<std::string> defines;              // --define VALUE pairs

    // Debug toggles (mirrored from phoskia::CompileOptions in Commit 3).
    // keepSources populates `CompiledShaderProgram::sources` in-memory.
    // dumpIntermediate writes the .sc files to disk under dumpDir.
    // When dumpDir is empty, dumpIntermediate is a no-op (caller is
    // expected to provide a valid directory). No automatic tempdir
    // fallback — explicit > implicit for debug paths.
    bool                     keepSources      = false;
    bool                     dumpIntermediate = false;
    std::string              dumpDir;
};

// Binding-info structs (`BGFXUniform`, `BGFXUniformBlock`,
// `BGFXStorageBuffer`, `BGFXTexture`) moved to AYShaderProgram.h in
// Phase 3.6 so that `CompiledShaderProgram` can hold them by value
// without dragging this header into every TU that just receives a
// compiled program. Reachable transitively via `#include
// "AYShaderProgram.h"` at the top of this header.

struct BGFXConvertResult {
    bool success = false;
    std::vector<detail::BGFXMaterialStages> materialStages;
    std::vector<detail::BGFXComputeStage> computeStages;
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

    // Phase 3.6 productization entry point. Runs `convertBGFX(program)`
    // internally to emit the per-stage .sc sources, then drives
    // `AYShadercDriver` once per stage to produce the .bin byte blobs
    // that go into `out.vsBin` / `out.fsBin` / `out.csBin`. Binding
    // metadata (UBOs / storage / uniforms / textures) is copied from
    // the internal BGFXConvertResult. Out-param form to sidestep the
    // Phase 3.2-pre MSVC SSO / NRVO bug (the same shape is large
    // enough that returning by value corrupts caller-stack memory
    // under some optimizer choices).
    //
    // Behaviour of the debug toggles in `opts`:
    //   * opts.keepSources == true → fill `out.sources` with the .sc
    //     text per stage (keyed by index; see `CompiledShaderProgram`
    //     comment for the table).
    //   * opts.dumpIntermediate == true → also write the .sc files to
    //     `opts.dumpDir`. Silently skipped when `dumpDir` is empty.
    //
    // On any failure (convertBGFX error / shaderc failure / shaderc
    // missing) the partial `out` is still returned with `success=false`
    // and a populated `errors` vector. The function does NOT throw —
    // shaderc-missing surfaces as `errors[0] = "AYShadercDriver: ..."`
    // rather than an exception propagating to the caller.
    void compileToBinary(const phoskia::ir::IRProgram& program,
                         const BGFXCompileOptions& opts,
                         CompiledShaderProgram& out);

    detail::BGFXMaterialStages convertMaterial(const phoskia::ir::IRMaterialDecl& material);
    detail::BGFXComputeStage convertComputeDecl(const phoskia::ir::IRComputeDecl& compute);

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
    // Phase 3.6: cached shaderc driver. Lazy-initialized on first
    // compileToBinary() call so that AYBGFXConverter construction
    // itself never throws when shaderc is missing — only the actual
    // compile attempt surfaces the diagnostic. Stored as unique_ptr
    // to forward-declared type (destructor runs in AYBGFXConverter.cpp
    // where the full definition is visible).
    std::unique_ptr<AYShadercDriver> _driver;
};

} // namespace ayt::shader
