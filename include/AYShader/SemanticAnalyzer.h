#pragma once
// AYShader\SemanticAnalyzer.h - Semantic analysis (scope, type checking) for Phoskia

#include "AYShader/Type.h"
#include "AYShader/Ast.h"
#include "AYShader/CompilerError.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    const std::vector<CompilerError>& warnings() const { return _warnings; }
    bool hasErrors() const { return _reporter.hasErrors(); }

    // Symbol table access
    bool isDefined(const std::string& name) const;
    std::shared_ptr<Type> getType(const std::string& name) const;

    // IR-H-01/IR-H-02/IR-H-04: hint the analyzer with the parser's current
    // cursor so errors at scope-less AST nodes (statements, declarations)
    // carry a real source location. The analyzer walks AST nodes
    // recursively; per-statement overrides (e.g. `line` field on
    // ReturnStmt) take precedence over this hint when set.
    void setCurrentLocation(int line, int column) {
        _currentLine = line;
        _currentColumn = column;
    }

private:
    void analyze(const Stmt& stmt);
    void analyzeMaterialDecl(const MaterialDecl& decl);
    void analyzePropertyDecl(const PropertyDecl& decl);
    void analyzeUniformDecl(const UniformDecl& decl);
    void analyzeTextureDecl(const TextureDecl& decl);
    void analyzeUniformBlockDecl(const UniformBlockDecl& decl);
    void analyzeVertexFunc(const VertexFunc& func);
    void analyzeFragmentFunc(const FragmentFunc& func);
    void analyzeShaderParam(const ShaderParam& param);
    void analyzeLetStmt(const LetStmt& stmt);
    void analyzeReturnStmt(const ReturnStmt& stmt);
    void analyzeIfStmt(const IfStmt& stmt);
    void analyzeForStmt(const ForStmt& stmt);
    std::shared_ptr<Type> analyzeExpr(const Expr& expr);

    // Walk an expression and collect every leaf IdentifierExpr (recursing
    // through binary/unary/call/member/index nodes, but skipping
    // MemberExpr's `.member` string and LiteralExpr's payload). Used by
    // analyzeExpr to surface "Undefined identifier" errors for nested
    // references like `a + mystery` where the leaf identifier is buried
    // inside a BinaryExpr.
    void collectIdentifiers(const Expr& expr,
                            std::vector<const IdentifierExpr*>& out);

    // Error / warning reporters.
    //   - error / warning default to the current location hint + UnknownIdentifier code.
    //   - Pass an explicit ErrorCode for accurate categorization
    //     (IR-H-01..04: TypeMismatch for unification / return / if-condition
    //     errors so downstream diagnostic tooling can group them).
    //   - Pass (line, column) explicitly when the AST node carries its own
    //     location — takes precedence over the current-location hint.
    void error(const std::string& message, int line, int column,
               ErrorCode code = ErrorCode::UnknownIdentifier);
    void warning(const std::string& message, int line, int column,
                 ErrorCode code = ErrorCode::UnknownIdentifier);

    // IR-M-04: walk the program once after the main pass to surface
    // bindings that were declared (let / property / uniform / param)
    // but never used. Warnings only — does NOT trip hasErrors().
    void emitUnusedBindingWarnings();

    TypeEnvironment& _env;
    CompilerErrorReporter _reporter;
    std::vector<CompilerError> _warnings;
    std::unordered_map<std::string, std::shared_ptr<Type>> _symbols;
    std::unordered_map<std::string, std::shared_ptr<Type>> _materialProperties;
    bool _inShaderFunc = false;
    // >0 while analyzing a fragment that declared MRT `out` targets.
    // Return→gl_FragColor is forbidden in that mode.
    size_t _fragmentMrtOutputCount = 0;

    // IR-H-01: current source-location hint. AST nodes that carry their
    // own line/column (ReturnStmt, IfStmt, BinaryExpr, ...) override
    // this hint in their corresponding analyze* methods.
    int _currentLine = 0;
    int _currentColumn = 0;

    // IR-M-04: track which let-bindings were *read* during analysis
    // (their initializer IdentifierExpr references show up via
    // collectIdentifiers). After the main pass, any let-stmt name not
    // present in this set is unused.
    std::unordered_set<std::string> _usedBindings;
    // Counter for how many times each let-binding was referenced.
    // Lets us skip the trivial `let x = 1.0; ...; x` pattern (one
    // read after declaration) vs. multi-statement usage.
    std::unordered_map<std::string, size_t> _bindingUseCount;

    // Track which let / property / uniform declarations we've seen.
    // After analyze() finishes, intersect with _usedBindings to find
    // orphans.
    struct DeclRecord {
        std::string name;
        int line;
        int column;
        bool isLet = false;       // false ⇒ property / uniform / param
    };
    std::vector<DeclRecord> _declaredBindings;

    // Program-level AST pointer (kept for IR-M-04 unused-walker; owned
    // by the caller — we only read it during emitUnusedBindingWarnings()).
    const Program* _currentProgram = nullptr;
};

} // namespace ayt::shader::phoskia
