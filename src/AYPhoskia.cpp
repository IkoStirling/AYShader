// AYPhoskia.cpp - Phoskia compiler implementation

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include <iostream>

namespace ayt::shader::phoskia
{

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

} // namespace ayt::shader::phoskia
