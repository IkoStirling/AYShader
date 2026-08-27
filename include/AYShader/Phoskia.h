#pragma once
// AYShader/Phoskia.h - Phoskia compiler main entry
//
// Phoskia is a high-level shader DSL. This header exposes the Phoskia
// Compiler class which orchestrates the compilation pipeline:
//   source -> tokenize -> parse -> (optional) analyze -> backend convert
//
// Backend converters are NOT owned by this class; they are registered
// via Compiler::registerBackend() so the core stays decoupled from any
// specific target platform.
//
// Phase 3.6 productization: the primary frontend entry is now
// `Compiler::compileToProgram(src)` which returns a
// `CompiledShaderProgram` (raw .bin bytes per stage + binding
// metadata). The legacy `Compiler::compile(src, CompileResult&)`
// still exists for callers that want the .sc text form, but its
// `output` field is empty under the default options and is marked
// `/// @deprecated`. Removal target: Phase 3.7.

#include "AYShader/Lexer.h"
#include "AYShader/Parser.h"
#include "AYShader/Ast.h"
#include "AYShader/Type.h"
#include "AYShader/TypeInference.h"
#include "AYShader/SemanticAnalyzer.h"
#include "AYShader/CompilerError.h"
#include "AYShader/IBackendConverter.h"
#include "AYShader/BuiltinFunctions.h"
#include "AYShader/Ir.h"
#include "AYShader/ShaderProgram.h"

#include <memory>
#include <string>
#include <variant>
#include <unordered_map>
#include <functional>

namespace ayt::shader
{
class ShaderResource;
class ShaderResourcePool;
struct BGFXCompileOptions;
}

namespace ayt::shader::phoskia
{

// Compilation result.
//
// DEPRECATED (Phase 3.6): the `output` field is the legacy .sc-text
// joiner shape. `Compiler::compile()` still populates it (for source
// compatibility — Commit 5 will rewrite the test assertions and stop
// relying on it). New code should call `Compiler::compileToProgram(src)`
// and read `CompiledShaderProgram::{vsBin,fsBin,csBin,sources}` instead.
// Removal target: Phase 3.7.
struct CompileResult {
    bool success = false;
    std::vector<CompilerError> errors;
    std::vector<std::string> warnings;
    std::shared_ptr<Program> ast;       // Pipeline keeps; future backends may want
    // Phase 3.2: removed `typeEnv` (never written, never read).
    // Phase 3.2: removed `ir` (was dropped in d6b7d2b to dodge SSO; still internal to runPipeline).
};

// Compilation options (Phase 4-R: frontend-facing only).
struct CompileOptions {
    std::vector<std::string> defines;
    bool keepSources = false;
    bool dumpIntermediate = false;
    std::string dumpDir;
};

// Factory function type for backend converters
using BackendFactory = std::function<std::unique_ptr<shader::IAYBackendConverter>()>;

// Main Phoskia compiler class
class Compiler {
public:
    Compiler();
    explicit Compiler(const CompileOptions& options);

    // Compile Phoskia source to the default backend's output.
    // Out-parameter form to avoid the MSVC SSO / NRVO corruption bug
    // documented at design.md §6.8. Mirrors Compiler::tokenize().
    //
    // Phase 3.6: still populates `out.output` (legacy .sc-text
    // joiner) for source compatibility — Commit 5 rewrites the
    // legacy test assertions that read it. New code should call
    // `compileToProgram(src)` for raw .bin bytes + binding metadata.
    void compile(const std::string& source, CompileResult& out);

    // Compile to a specific registered backend.
    void compileToBackend(const std::string& source,
                          const std::string& backendName,
                          CompileResult& out);

    // ------------------------------------------------------------------
    // Phase 3.6: productization entry point. Drives the full pipeline
    // (tokenize → parse → IR → backend emit → shaderc) and returns
    // a CompiledShaderProgram with per-stage .bin bytes + binding
    // metadata.
    //
    // Two return-by-value overloads (default opts / explicit opts)
    // and one out-param overload. The out-param form is the safest
    // path on MSVC — the return-by-value forms rely on NRVO and may
    // corrupt caller-stack memory under some optimizer choices (the
    // Phase 3.2-pre SSO bug). For new code prefer the out-param form.
    // ------------------------------------------------------------------

    // Default options.
    CompiledShaderProgram compileToProgram(const std::string& source);

    // Explicit options. `opts` is taken by value because we apply
    // env-var overrides before dispatching (true-wins OR) — easier
    // to reason about on a local copy.
    CompiledShaderProgram compileToProgram(const std::string& source,
                                           const CompileOptions& opts);

    // SSO-safe out-param overload.
    void compileToProgram(const std::string& source,
                          const CompileOptions& opts,
                          CompiledShaderProgram& out);

    // Phase 4-B: engine-side BGFX/shaderc config owned by ShaderResourcePool.
    void compileToProgram(const std::string& source,
                          const CompileOptions& opts,
                          const shader::BGFXCompileOptions& engineOpts,
                          CompiledShaderProgram& out);

    // Phase 4-C: compile + wire-up in one call via an active pool.
    shader::ShaderResource compileToShaderResource(const std::string& source,
                                                   shader::ShaderResourcePool& pool);
    shader::ShaderResource compileToShaderResource(const std::string& source,
                                                   const CompileOptions& opts,
                                                   shader::ShaderResourcePool& pool);

    // Register a backend converter factory (e.g. "bgfx" -> AYBGFXConverter).
    void registerBackend(const std::string& name, BackendFactory factory);

    // Individual phases (for tests / advanced use)
    // Out-parameter to avoid returning std::vector<Token> by value (see
    // Lexer::tokenize for the same rationale — MSVC SSO string move bug).
    void tokenize(const std::string& source, std::vector<Token>& out);
    std::unique_ptr<Program> parse(const std::vector<Token>& tokens);
    std::shared_ptr<TypeEnvironment> analyzeSemantics(Program& program);

    bool generateIr(const std::string& source,
                  const CompileOptions& opts,
                  ir::IRProgram& out,
                  std::vector<std::string>& errors);

private:
    static constexpr const char* kDefaultBackend = "bgfx";
    static constexpr bool kEnableTypeInference = true;
    static constexpr bool kEnableSemanticAnalysis = true;

    void runPipeline(const std::string& source,
                     const std::string& backendName,
                     CompileResult& out);

    // Phase 3.6: shared helper that runs the tokenize → parse → IR
    // front-end and (on success) drives the registered backend's
    // `compileToBinary` to populate `out`. Called by all three
    // `compileToProgram` overloads.
    void runToProgram(const std::string& source,
                      const CompileOptions& opts,
                      CompiledShaderProgram& out);

    void runToProgram(const std::string& source,
                      const CompileOptions& opts,
                      const shader::BGFXCompileOptions& engineOpts,
                      CompiledShaderProgram& out);

    CompileOptions _options;
    CompilerErrorReporter _errorReporter;
    std::vector<std::string> _warningMessages;
    std::shared_ptr<TypeEnvironment> _typeEnv;
    std::unordered_map<std::string, BackendFactory> _backends;
};

// ----------------------------------------------------------------------------
// IR (Intermediate Representation) — Phase 3.1
// ----------------------------------------------------------------------------
// The Phoskia IR lives in `ayt::shader::phoskia::ir` (see include/AYShader/Ir.h).
// It is a 1:1 mirror of the AST with `resolvedType` pre-attached to every
// expression. Backends consume the IR instead of the AST. Future SSA-style
// instruction stream + cross-backend optimization passes are Phase 3.x
// additions on top of this IR substrate.

namespace ir {
    struct IRProgram;     // forward decl so AYPhoskia.cpp can use the
    class  IRGenerator;   // type without dragging in AYShader/Ir.h transitively
                          // (some toolchains complain about incomplete
                          // types when only forward decls are visible).
}

} // namespace ayt::shader::phoskia
