// AYPhoskia.cpp - Phoskia compiler implementation

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include "AYShaderResourcePool.h"
#include <cctype>
#include <cstdlib>
#include <iostream>

namespace ayt::shader::phoskia
{

namespace {

// Parse "1" / "true" / "yes" / "on" (case-insensitive) as true; anything
// else as false. Empty string is false. The env-var precedence tests
// in Test_ShadercDriver.cpp / Test_CompileOptions.cpp lock this contract.
bool parseEnvBool(const char* v) {
    if (!v || !*v) return false;
    // Case-insensitive compare against the truthy tokens.
    std::string s(v);
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

// Apply env-var overrides to a CompileOptions copy. true-wins OR:
// any source wanting a toggle ON flips it ON. Used by compileToProgram
// to fold AY_PHOSKIA_KEEP_SOURCES / AY_PHOSKIA_DUMP_SC into the
// caller's opts. Returns a new CompileOptions — opts is not mutated
// because callers may want to keep their original value (e.g. for
// logging).
CompileOptions applyEnvOverrides(CompileOptions opts) {
    if (parseEnvBool(std::getenv("AY_PHOSKIA_KEEP_SOURCES"))) {
        opts.keepSources = true;
    }
    if (parseEnvBool(std::getenv("AY_PHOSKIA_DUMP_SC"))) {
        opts.dumpIntermediate = true;
    }
    return opts;
}

} // namespace

Compiler::Compiler() {
    _typeEnv = std::make_shared<TypeEnvironment>();
    // Register the only backend that ships with Phase 1.
    registerBackend("bgfx", []() -> std::unique_ptr<shader::IAYBackendConverter> {
        // std::unique_ptr<AYBGFXConverter> doesn't implicitly convert
        // to std::unique_ptr<IAYBackendConverter> (different
        // deleters), so wrap explicitly.
        return std::unique_ptr<shader::IAYBackendConverter>(
            new shader::AYBGFXConverter());
    });
}

Compiler::Compiler(const CompileOptions& options) : _options(options) {
    _typeEnv = std::make_shared<TypeEnvironment>();
    registerBackend("bgfx", []() -> std::unique_ptr<shader::IAYBackendConverter> {
        return std::unique_ptr<shader::IAYBackendConverter>(
            new shader::AYBGFXConverter());
    });
}

void Compiler::registerBackend(const std::string& name, BackendFactory factory) {
    _backends[name] = std::move(factory);
}

void Compiler::compile(const std::string& source, CompileResult& out) {
    runPipeline(source, _options.targetBackend, out);
}

void Compiler::compileToBackend(const std::string& source,
                                 const std::string& backendName,
                                 CompileResult& out) {
    runPipeline(source, backendName, out);
}

CompiledShaderProgram Compiler::compileToProgram(const std::string& source) {
    CompiledShaderProgram out;
    shader::BGFXCompileOptions engineDefaults;
    runToProgram(source, applyEnvOverrides(_options), engineDefaults, out);
    return out;
}

CompiledShaderProgram Compiler::compileToProgram(const std::string& source,
                                                 const CompileOptions& opts) {
    CompiledShaderProgram out;
    compileToProgram(source, opts, out);
    return out;
}

void Compiler::compileToProgram(const std::string& source,
                                const CompileOptions& opts,
                                CompiledShaderProgram& out) {
    shader::BGFXCompileOptions engineDefaults;
    runToProgram(source, applyEnvOverrides(opts), engineDefaults, out);
}

void Compiler::compileToProgram(const std::string& source,
                                const CompileOptions& opts,
                                const shader::BGFXCompileOptions& engineOpts,
                                CompiledShaderProgram& out) {
    runToProgram(source, applyEnvOverrides(opts), engineOpts, out);
}

shader::ShaderResource Compiler::compileToShaderResource(
    const std::string& source,
    shader::ShaderResourcePool& pool)
{
    return pool.compile(source);
}

shader::ShaderResource Compiler::compileToShaderResource(
    const std::string& source,
    const CompileOptions& opts,
    shader::ShaderResourcePool& pool)
{
    return pool.compile(source, opts);
}

void Compiler::tokenize(const std::string& source, std::vector<Token>& out) {
    Lexer lexer(source);
    lexer.tokenize(out);
}

std::unique_ptr<Program> Compiler::parse(const std::vector<Token>& tokens) {
    Parser parser(tokens);
    return parser.parse();
}

std::shared_ptr<TypeEnvironment> Compiler::analyzeSemantics(Program& program) {
    AYSemanticAnalyzer analyzer(*_typeEnv);
    analyzer.analyze(program);
    // DEBUG: retained — surfaces semantic-error propagation in the global
    // reporter for error-recovery tests in Phase 2 #1.
    std::cerr << "[Compiler::analyzeSemantics] analyzer.errors().size()="
              << analyzer.errors().size() << "\n";
    // Bubble analyzer errors back through the global reporter.
    for (const auto& e : analyzer.errors()) {
        _errorReporter.error(e.code, e.message, e.line, e.column);
    }
    return _typeEnv;
}

void Compiler::runPipeline(const std::string& source,
                           const std::string& backendName,
                           CompileResult& out) {
    out = CompileResult{};
    _errorReporter.clear();

    // 1) Tokenize
    std::vector<Token> tokens;
    try {
        tokenize(source, tokens);
    } catch (const std::exception& e) {
        _errorReporter.error(ErrorCode::UnexpectedToken, e.what(), 0, 0);
        out.errors = _errorReporter.errors();
        return;
    }

    // 2) Parse
    Parser parser(tokens);
    std::unique_ptr<Program> ast;
    try {
        ast = parser.parse();
    } catch (const std::exception& e) {
        _errorReporter.error(ErrorCode::InvalidExpression, e.what(), 0, 0);
        out.errors = _errorReporter.errors();
        return;
    }
    // Forward parser diagnostics into the compiler-wide reporter.
    for (const auto& e : parser.errors()) {
        _errorReporter.error(e.code, e.message, e.line, e.column);
    }
    out.ast = std::move(ast);
    if (_errorReporter.hasErrors()) {
        out.errors = _errorReporter.errors();
        return;
    }

    // 3) Optional semantic analysis
    if (_options.enableSemanticAnalysis) {
        analyzeSemantics(*out.ast);
    }

    // 4) Type inference — Phase 2 Step 2: when enabled, walk every
    //    material body and verify each return value has type vec4.
    //    We use a fresh TypeInference per pipeline so the engine's
    //    internal type-var list doesn't leak between materials.
    if (_options.enableTypeInference) {
        TypeInference inference(*_typeEnv);
        // Build the builtin env the analyzer normally builds, so the
        // engine's identifier lookup finds `vec4` / `sample` / etc.
        //
        // Phase 3.3 Block 3: skip 0-arg builtins (thread_id / group_id
        // / dispatch_id). They are exposed in Phoskia source as bare
        // identifiers (`thread_id.x`); the inference engine's
        // builtin-registry fallback returns the return type (uvec3)
        // directly when the env doesn't have the name. Adding them as
        // FunctionType here would shadow that fallback and the swizzle
        // inference would see a FunctionType rather than uvec3.
        for (const auto& name : BuiltinFunctionRegistry::instance().getAllFunctionNames()) {
            auto func = BuiltinFunctionRegistry::instance().getFunction(name);
            if (func) {
                if (func->paramTypes.empty()) continue;
                _typeEnv->addFunction(name,
                    std::make_shared<FunctionType>(func->paramTypes, func->returnType));
            }
        }

        int returnViolations = 0;
        for (const auto& d : out.ast->declarations) {
            auto* mat = dynamic_cast<const MaterialDecl*>(d.get());
            if (!mat) continue;

            auto checkBody = [&](const std::vector<StmtPtr>& body) {
                for (const auto& s : body) {
                    auto* ret = dynamic_cast<const ReturnStmt*>(s.get());
                    if (!ret || !ret->value) continue;
                    auto t = inference.infer(*ret->value);

                    // Resolve any TypeVar wrapper.
                    std::shared_ptr<Type> concrete = t;
                    while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
                        if (tv->hasSolution()) concrete = tv->getSolution();
                        else break;
                    }
                    auto vec4 = BuiltinTypes::Vec4();
                    if (concrete && !concrete->equals(*vec4)) {
                        auto dyn = BuiltinTypes::Dynamic;
                        bool unresolved =
                            std::dynamic_pointer_cast<TypeVar>(concrete) != nullptr ||
                            (dyn && concrete->equals(*dyn));
                        if (!unresolved) {
                            ++returnViolations;
                            _errorReporter.error(
                                ErrorCode::TypeMismatch,
                                "Return type must be vec4; got " + concrete->toString(),
                                0, 0);
                        }
                    }
                }
            };

            for (const auto& inner : mat->declarations) {
                if (auto* vs = dynamic_cast<const VertexFunc*>(inner.get())) {
                    checkBody(vs->body);
                } else if (auto* fs = dynamic_cast<const FragmentFunc*>(inner.get())) {
                    checkBody(fs->body);
                }
            }
        }

        if (returnViolations > 0) {
            // Don't fail the pipeline — the BGFX backend may still produce
            // a useful output. But the diagnostics are visible in
            // out.errors so callers can react.
            (void)returnViolations;
        }
    }

    if (_errorReporter.hasErrors() && !out.ast) {
        out.errors = _errorReporter.errors();
        return;
    }

    // 5) Backend conversion
    auto it = _backends.find(backendName);
    if (it == _backends.end()) {
        _errorReporter.error(ErrorCode::UnexpectedToken,
                             "Unknown backend: " + backendName, 0, 0);
        out.errors = _errorReporter.errors();
        return;
    }

    auto backend = it->second();
    if (!backend) {
        _errorReporter.error(ErrorCode::InvalidOperation,
                             "Backend factory for '" + backendName +
                             "' returned null", 0, 0);
        out.errors = _errorReporter.errors();
        return;
    }

    // Phase 3.1: lower the AST to the Phoskia IR before backend
    // dispatch. Backends consume IR instead of AST. The IRGenerator
    // gracefully handles a missing TypeEnvironment by running
    // TypeInference on demand for each expression. The IR is held
    // locally — it is not stored on `out`; callers that need it can
    // call `ir::IRGenerator::generate(*out.ast)` directly.
    ir::IRProgram irProgram = [&] {
        ir::IRGenerator gen;
        return gen.generate(*out.ast, _typeEnv);
    }();

    auto backendResult = backend->convert(irProgram);
    out.output = std::move(backendResult.output);
    // Promote backend string errors to CompilerError entries.
    for (const auto& msg : backendResult.errors) {
        out.errors.emplace_back(ErrorCode::InvalidOperation, msg, 0, 0);
    }
    out.warnings.insert(out.warnings.end(),
                        backendResult.warnings.begin(),
                        backendResult.warnings.end());
    out.success = backendResult.success;
}

// Phase 3.6: shared implementation for the three `compileToProgram`
// overloads. The frontend-facing API surface (out-param vs return-value
// vs default-opts) is kept minimal here — all the actual work lives in
// this helper.
//
// Pipeline:
//   1. tokenize → parse (mirrors runPipeline's front-end).
//   2. Optional semantic analysis (CompileOptions::enableSemanticAnalysis).
//   3. Generate IR via IRGenerator.
//   4. Construct AYBGFXConverter directly and call compileToBinary.
//      (compileToProgram is BGFX-only for now; other backends are
//      Phase 5+. When a non-BGFX backend registers a compileToBinary-
//      equivalent we'll add an IAYBackendConverter::compileToBinary
//      virtual and dispatch here.)
//   5. Surface any front-end errors as CompilerError-like strings in
//      out.errors and set success=false.
//
// The `opts` parameter has already had env-var overrides applied by
// the public overloads (applyEnvOverrides) — we don't re-read env here.
void Compiler::runToProgram(const std::string& source,
                            const CompileOptions& opts,
                            CompiledShaderProgram& out) {
    shader::BGFXCompileOptions engineDefaults;
    runToProgram(source, opts, engineDefaults, out);
}

void Compiler::runToProgram(const std::string& source,
                            const CompileOptions& opts,
                            const shader::BGFXCompileOptions& engineOpts,
                            CompiledShaderProgram& out) {
    out = CompiledShaderProgram{};
    _errorReporter.clear();

    // 1) Tokenize
    std::vector<Token> tokens;
    try {
        tokenize(source, tokens);
    } catch (const std::exception& e) {
        out.errors.push_back(e.what());
        out.success = false;
        return;
    }

    // 2) Parse
    Parser parser(tokens);
    std::unique_ptr<Program> ast;
    try {
        ast = parser.parse();
    } catch (const std::exception& e) {
        out.errors.push_back(e.what());
        out.success = false;
        return;
    }
    // Forward parser diagnostics ONLY when the parse didn't produce an
    // AST — when `ast` is non-null the parser succeeded (its
    // try-everything mode logs recovery noise to stderr even on a
    // successful parse, and we don't want that to look like an error
    // to compileToProgram callers).
    if (!ast) {
        for (const auto& e : parser.errors()) {
            out.errors.push_back(e.message);
        }
        out.success = false;
        return;
    }

    // 3) Optional semantic analysis (same gates as runPipeline).
    if (opts.enableSemanticAnalysis && ast) {
        analyzeSemantics(*ast);
        for (const auto& e : _errorReporter.errors()) {
            out.errors.push_back(e.message);
        }
        if (!out.errors.empty()) {
            out.success = false;
            return;
        }
    }

    // 4) Generate IR.
    ir::IRProgram irProgram = [&] {
        ir::IRGenerator gen;
        return gen.generate(*ast, _typeEnv);
    }();

    // 5) Drive the BGFX backend.
    //
    // Map phoskia::CompileOptions → BGFXCompileOptions. We pass
    // through keepSources / dumpIntermediate / dumpDir / includeDirs.
    // (Backend-specific fields like platform / profile default to
    // BGFXCompileOptions' own defaults — frontend doesn't need to
    // know about them. Phase 4 may add `CompileOptions::bgfxPlatform`
    // if frontend needs to override.)
    shader::AYBGFXConverter converter;
    shader::BGFXCompileOptions bgfxOpts = engineOpts;
    bgfxOpts.keepSources      = opts.keepSources;
    bgfxOpts.dumpIntermediate = opts.dumpIntermediate;
    bgfxOpts.dumpDir          = opts.dumpDir;
    if (!opts.includeDirs.empty()) {
        bgfxOpts.includeDirs = opts.includeDirs;
    }

    converter.compileToBinary(irProgram, bgfxOpts, out);
}

} // namespace ayt::shader::phoskia
