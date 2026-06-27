#pragma once
// AYSemanticAnalyzer.h - Semantic analysis (scope, type checking) for Phoskia

#include "AYType.h"
#include "AYAst.h"
#include "AYCompilerError.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

namespace ayt::shader::phoskia
{

class AYSemanticAnalyzer {
public:
    explicit AYSemanticAnalyzer(TypeEnvironment& env);

    // Analyze a program. Returns true on success (no errors).
    bool analyze(const Program& program);

    // Diagnostics collected during analysis.
    const std::vector<CompilerError>& errors() const { return _reporter.errors(); }
    bool hasErrors() const { return _reporter.hasErrors(); }

    // Symbol table access
    bool isDefined(const std::string& name) const;
    std::shared_ptr<Type> getType(const std::string& name) const;

private:
    void analyze(const Stmt& stmt);
    void analyzeMaterialDecl(const MaterialDecl& decl);
    void analyzePropertyDecl(const PropertyDecl& decl);
    void analyzeUniformDecl(const UniformDecl& decl);
    void analyzeTextureDecl(const TextureDecl& decl);
    void analyzeVertexFunc(const VertexFunc& func);
    void analyzeFragmentFunc(const FragmentFunc& func);
    void analyzeShaderParam(const ShaderParam& param);
    void analyzeLetStmt(const LetStmt& stmt);
    void analyzeReturnStmt(const ReturnStmt& stmt);
    void analyzeIfStmt(const IfStmt& stmt);
    void analyzeForStmt(const ForStmt& stmt);
    std::shared_ptr<Type> analyzeExpr(const Expr& expr);

    void error(const std::string& message, int line, int column);
    void warning(const std::string& message, int line, int column);

    TypeEnvironment& _env;
    CompilerErrorReporter _reporter;
    std::unordered_map<std::string, std::shared_ptr<Type>> _symbols;
    std::unordered_map<std::string, std::shared_ptr<Type>> _materialProperties;
    bool _inShaderFunc = false;
};

} // namespace ayt::shader::phoskia
