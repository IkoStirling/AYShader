#pragma once
// AYPhoskia.h - Phoskia compiler main entry
//
// Phoskia is a high-level shader DSL. This header exposes the Phoskia
// Compiler class which orchestrates the compilation pipeline:
//   source -> tokenize -> parse -> (optional) analyze -> backend convert
//
// Backend converters are NOT owned by this class; they are registered
// via Compiler::registerBackend() so the core stays decoupled from any
// specific target platform.

#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYType.h"
#include "AYTypeInference.h"
#include "AYSemanticAnalyzer.h"
#include "AYCompilerError.h"
#include "IAYBackendConverter.h"
#include "AYBuiltinFunctions.h"
#include "AYIr.h"

#include <memory>
#include <string>
#include <variant>
#include <unordered_map>
#include <functional>

namespace ayt::shader::phoskia
{

// Compilation result.
//
// Populated by `Compiler::compile` / `Compiler::compileToBackend` via
// an out-parameter; see design.md §6.8 for the rationale (out-param
// form roots out an MSVC SSO / NRVO interaction when this struct
// grew past a size threshold in Phase 3.1).
struct CompileResult {
    bool success = false;
    std::string output;                  // Backend output (e.g. .sc text)
    std::vector<CompilerError> errors;
    std::vector<std::string> warnings;
    std::shared_ptr<Program> ast;       // Pipeline keeps; future backends may want
    // Phase 3.2: removed `typeEnv` (never written, never read).
    // Phase 3.2: removed `ir` (was dropped in d6b7d2b to dodge SSO; still internal to runPipeline).
};

// Compilation options
struct CompileOptions {
    bool enableTypeInference = false;    // Phase 2 Step 2: opt-in. Off by default
                                         // because not every Phoskia snippet
                                         // currently type-checks (mixed vector
                                         // / scalar arithmetic, etc.).
    bool enableSemanticAnalysis = false; // Phase 2 Step 2: opt-in. The
                                         // analyzer enforces strict checks
                                         // (vec4 returns, bool if-conds,
                                         // swizzle validity) that some
                                         // existing Phase 1 snippets bypass.
    bool strictMode = false;
    std::string targetBackend = "bgfx";
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
    void compile(const std::string& source, CompileResult& out);

    // Compile to a specific registered backend.
    void compileToBackend(const std::string& source,
                          const std::string& backendName,
                          CompileResult& out);

    // Register a backend converter factory (e.g. "bgfx" -> AYBGFXConverter).
    void registerBackend(const std::string& name, BackendFactory factory);

    // Individual phases (for tests / advanced use)
    // Out-parameter to avoid returning std::vector<Token> by value (see
    // Lexer::tokenize for the same rationale — MSVC SSO string move bug).
    void tokenize(const std::string& source, std::vector<Token>& out);
    std::unique_ptr<Program> parse(const std::vector<Token>& tokens);
    std::shared_ptr<TypeEnvironment> analyzeSemantics(Program& program);

private:
    void runPipeline(const std::string& source,
                     const std::string& backendName,
                     CompileResult& out);

    CompileOptions _options;
    CompilerErrorReporter _errorReporter;
    std::shared_ptr<TypeEnvironment> _typeEnv;
    std::unordered_map<std::string, BackendFactory> _backends;
};

// ----------------------------------------------------------------------------
// IR (Intermediate Representation) — Phase 3.1
// ----------------------------------------------------------------------------
// The Phoskia IR lives in `ayt::shader::phoskia::ir` (see include/AYIr.h).
// It is a 1:1 mirror of the AST with `resolvedType` pre-attached to every
// expression. Backends consume the IR instead of the AST. Future SSA-style
// instruction stream + cross-backend optimization passes are Phase 3.x
// additions on top of this IR substrate.

namespace ir {
    struct IRProgram;     // forward decl so AYPhoskia.cpp can use the
    class  IRGenerator;   // type without dragging in AYIr.h transitively
                          // (some toolchains complain about incomplete
                          // types when only forward decls are visible).
}

} // namespace ayt::shader::phoskia
