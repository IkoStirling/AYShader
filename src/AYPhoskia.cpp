// AYPhoskia.cpp - Phoskia compiler implementation

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"

namespace ayt::shader::phoskia
{

Compiler::Compiler() {
    _typeEnv = std::make_shared<TypeEnvironment>();
    // Register the only backend that ships with Phase 1.
    registerBackend("bgfx", []() -> std::unique_ptr<shader::IAYBackendConverter> {
        return std::make_unique<shader::AYBGFXConverter>();
    });
}

Compiler::Compiler(const CompileOptions& options) : _options(options) {
    _typeEnv = std::make_shared<TypeEnvironment>();
    registerBackend("bgfx", []() -> std::unique_ptr<shader::IAYBackendConverter> {
        return std::make_unique<shader::AYBGFXConverter>();
    });
}

void Compiler::registerBackend(const std::string& name, BackendFactory factory) {
    _backends[name] = std::move(factory);
}

CompileResult Compiler::compile(const std::string& source) {
    return runPipeline(source, _options.targetBackend);
}

CompileResult Compiler::compileToBackend(const std::string& source, const std::string& backendName) {
    return runPipeline(source, backendName);
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
    // Bubble analyzer errors back through the global reporter.
    for (const auto& e : analyzer.errors()) {
        _errorReporter.error(e.code, e.message, e.line, e.column);
    }
    return _typeEnv;
}

CompileResult Compiler::runPipeline(const std::string& source, const std::string& backendName) {
    CompileResult result;
    _errorReporter.clear();

    // 1) Tokenize
    std::vector<Token> tokens;
    try {
        tokenize(source, tokens);
    } catch (const std::exception& e) {
        _errorReporter.error(ErrorCode::UnexpectedToken, e.what(), 0, 0);
        result.errors = _errorReporter.errors();
        return result;
    }

    // 2) Parse
    Parser parser(tokens);
    std::unique_ptr<Program> ast;
    try {
        ast = parser.parse();
    } catch (const std::exception& e) {
        _errorReporter.error(ErrorCode::InvalidExpression, e.what(), 0, 0);
        result.errors = _errorReporter.errors();
        return result;
    }
    // Forward parser diagnostics into the compiler-wide reporter.
    for (const auto& e : parser.errors()) {
        _errorReporter.error(e.code, e.message, e.line, e.column);
    }
    result.ast = std::move(ast);
    if (hasErrors()) {
        result.errors = _errorReporter.errors();
        return result;
    }

    // 3) Optional semantic analysis
    if (_options.enableSemanticAnalysis) {
        analyzeSemantics(*result.ast);
    }

    // 4) Type inference (currently piggybacks on semantic analysis; not enabled by default)
    if (_options.enableTypeInference) {
        TypeInference inference(*_typeEnv);
        // (Pass is best-effort in Phase 1; not all expressions are walked.)
    }

    if (hasErrors()) {
        result.errors = _errorReporter.errors();
        return result;
    }

    // 5) Backend conversion
    auto it = _backends.find(backendName);
    if (it == _backends.end()) {
        _errorReporter.error(ErrorCode::UnexpectedToken,
                             "Unknown backend: " + backendName, 0, 0);
        result.errors = _errorReporter.errors();
        return result;
    }

    auto backend = it->second();
    auto backendResult = backend->convert(*result.ast);
    result.output = std::move(backendResult.output);
    // Promote backend string errors to CompilerError entries.
    for (const auto& msg : backendResult.errors) {
        result.errors.emplace_back(ErrorCode::InvalidOperation, msg, 0, 0);
    }
    result.warnings.insert(result.warnings.end(),
                           backendResult.warnings.begin(),
                           backendResult.warnings.end());
    result.success = backendResult.success;
    return result;
}

} // namespace ayt::shader::phoskia
