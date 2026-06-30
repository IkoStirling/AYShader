#pragma once
// AYShaderProgram.h - Compiled shader program
//
// Phase 3.6 productization split this file into several co-existing
// shapes:
//
//   * Binding-info structs (BGFXUniform / BGFXUniformBlock /
//     BGFXStorageBuffer / BGFXTexture) — backend-agnostic metadata
//     about what bindings a shader declares. Moved here from
//     AYBGFXConverter.h so that `CompiledShaderProgram` (below) can
//     hold them by value without dragging AYBGFXConverter into every
//     TU that just wants to receive a compiled program.
//
//   * `CompiledShaderProgram` — pre-wire-up raw-bytes form. The
//     frontend gets one of these per `Compiler::compileToProgram(src)`
//     call. It carries the per-stage .bin bytes (what shaderc
//     produced), the binding metadata, and an opt-in in-memory map
//     of the .sc source for debug. This is the form Phase 4 wires up
//     to bgfx::createShader/createProgram.
//
//   * `ShaderProgram` — bgfx-handle form (the historical Phase 1
//     type). Still used by `ShaderCache` and by Phase 4 wire-up code
//     that constructs bgfx::ShaderHandle / bgfx::ProgramHandle values.
//
// See design.md §8.4 for the locked API contract.

#include <bgfx/bgfx.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ayt::shader
{

// ----------------------------------------------------------------------------
// Binding-info structs (moved from AYBGFXConverter.h in Phase 3.6).
//
// These describe the uniforms / texture bindings a backend extracted
// from the Phoskia source. They are backend-agnostic in name but the
// naming follows bgfx conventions because that's the only backend in
// 3.6. Future backends (HLSL / WGSL / GLSL-Native) will either reuse
// these same names or replace them with a polymorphic interface
// (Phase 5+).
// ----------------------------------------------------------------------------

// One shader-side uniform / property / texture-binding declaration.
struct BGFXUniform {
    std::string name;
    std::string type;
    uint8_t count = 1;
    // Phase 3.4: `blockName == ""` indicates a legacy `uniform T x;`
    // declaration; otherwise this is a UBO field and `blockName` /
    // `blockBinding` identify the parent block.
    std::string blockName;
    int blockBinding = -1;
};

// Texture sampler binding. `textureType` is the bgfx GLSL macro
// ("sampler2D" / "sampler3D" / "samplerCube").
struct BGFXTexture {
    std::string name;
    uint8_t binding = 0;
    std::string textureType = "sampler2D";
};

// Phase 3.4: uniform buffer object (GLSL `uniform Name { ... } name;`).
// `sizeBytes` is the std140 block size, computed by the GLSL compiler
// (captured at runtime via `bgfx::getUniformBlockSize` or similar —
// Phase 3.4 leaves it 0; host populates after the shader is compiled).
// `fieldNames` mirrors the AST field order so the host can compute
// per-field std140 offsets if it needs them.
struct BGFXUniformBlock {
    std::string name;
    int binding = -1;
    size_t sizeBytes = 0;
    std::vector<std::string> fieldNames;
};

// Phase 3.5-A: storage buffer (GLSL `buffer Name { T data[]; } Name;`).
// Distinct from BGFXUniformBlock because storage and uniform buffers
// live in different descriptor sets in the underlying graphics API.
struct BGFXStorageBuffer {
    std::string name;
    int binding = -1;
    std::string elementType;
};

// ----------------------------------------------------------------------------
// Phase 3.6: pre-wire-up raw-bytes form.
//
// `vsBin` / `fsBin` / `csBin` carry the bytes shaderc produced from
// AYBGFXConverter's emitted .sc sources. The frontend (host engine)
// hands them to bgfx::createShader in Phase 4 to build the final
// `ShaderProgram` (the bgfx-handle class further down).
//
// `sources` is filled only when the caller opted in via
// `CompileOptions.keepSources = true` (or env var
// `AY_PHOSKIA_KEEP_SOURCES=1`). The keys are:
//   "vs_<i>.sc"            vertex stage for material i
//   "fs_<i>.sc"            fragment stage for material i
//   "cs_<i>.sc"            compute stage for compute i
//   "varying.def.sc"       shared varying/attribute table (single key)
// Keys are index-based and not stable across material-renames — the
// frontend should not pin them in any user-visible API. They exist for
// debug (IDE preview / diff tools) and for the test-only joiner that
// reconstructs the legacy .sc-text shape for golden-file comparison.
//
// `errors` accumulates both backend-emit errors (e.g. duplicate UBO
// binding) and shaderc stderr text (compile failure). `warnings`
// holds non-fatal diagnostic noise (e.g. dumpIntermediate write
// failures — the .bin bytes are still produced). An empty `errors`
// vector together with `success == true` is the canonical "ok" signal.
// ----------------------------------------------------------------------------
struct CompiledShaderProgram {
    bool                       success = false;
    std::vector<std::string>   errors;
    std::vector<std::string>   warnings;

    // Per-stage binary shader bytes. Empty when the corresponding stage
    // is not present in the source (e.g. compute-only program has empty
    // vsBin / fsBin; material-only program has empty csBin).
    //
    // Single-vector shape: a multi-material source compiles every
    // material's stages but only the LAST material's bytes are kept.
    // Earlier materials' .bin blobs are not preserved. The debug
    // `sources` map below still carries every material's .sc text, so
    // no information is lost on the debug path. Phase 4 will likely
    // reshape this to vector<vector<uint8_t>> when bgfx wire-up needs
    // multiple materials' binaries simultaneously.
    std::vector<uint8_t>       vsBin;
    std::vector<uint8_t>       fsBin;
    std::vector<uint8_t>       csBin;

    // Binding metadata. The frontend iterates these to wire up
    // `bgfx::createUniform(name, type, count)` and the per-draw
    // `bgfx::setUniform(handle, ptr, sizeof(...))` calls.
    std::vector<BGFXUniformBlock>    uniformBlocks;
    std::vector<BGFXStorageBuffer>   storageBuffers;
    std::vector<BGFXUniform>         uniforms;
    std::vector<BGFXTexture>         textures;

    // Debug-only in-memory .sc sources. Empty unless the caller set
    // `CompileOptions.keepSources = true` (or env `AY_PHOSKIA_KEEP_SOURCES=1`).
    // See the table above for the key format.
    std::map<std::string, std::string>   sources;
};

// ----------------------------------------------------------------------------
// Legacy Phase 1 type — bgfx-handle form. Lives on for ShaderCache
// and any direct user of `bgfx::ShaderHandle` / `bgfx::ProgramHandle`.
// Phase 4 wire-up is expected to build this from a `CompiledShaderProgram`
// by feeding the .bin bytes to `bgfx::createShader`.
//
// NOT marked `[[deprecated]]` because the class still has legitimate
// uses (cache + Phase 4 wire-up). The deprecation target is
// `BGFXShaderFiles::{vs,fs,varyingDef}` and `BGFXComputeFile::cs` in
// AYBGFXConverter.h, which carry .sc text instead of .bin bytes and
// are the ones frontend code should stop reading.
// ----------------------------------------------------------------------------

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