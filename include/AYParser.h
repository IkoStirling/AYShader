#pragma once
// AYParser.h - Parser for Phoskia shader language

#include "AYToken.h"
#include "AYAst.h"
#include "AYCompilerError.h"
#include <array>
#include <memory>
#include <vector>

namespace ayt::shader::phoskia
{

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    // Parses the full program. Always returns a Program (may be partial on errors).
    // Check errors() / hasErrors() for diagnostics.
    std::unique_ptr<Program> parse();

    // Diagnostics collected during parsing.
    const std::vector<CompilerError>& errors() const { return _reporter.errors(); }
    bool hasErrors() const { return _reporter.hasErrors(); }

private:
    // Recursive descent parsing
    std::unique_ptr<Stmt> parseStatement();
    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parsePrimary();
    std::unique_ptr<Expr> parseBinary(int precedence = 0);
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parseCall();
    std::unique_ptr<Expr> parseMember();
    std::unique_ptr<Expr> parseIndex();

    std::unique_ptr<Stmt> parseMaterialDecl();
    // Phase 3.3 Block 2: the no-attribute parseComputeDecl() helper
    // was retired in favour of parseComputeDeclWithAttributes() which
    // consumes both shapes (with and without `[numthreads(...)]`).
    std::unique_ptr<ComputeDecl> parseComputeDeclWithAttributes(
        std::array<uint32_t, 3>& outNumThreads, bool& outHasNumThreads);
    // Helper: skip the inner contents of an unknown bracketed
    // attribute (used when parseComputeDeclWithAttributes encounters
    // an attribute name it doesn't recognise). Consumes tokens up to
    // (but not including) the closing ']'.
    void skipBracketedAttributeBody();
    std::unique_ptr<Stmt> parsePropertyDecl();
    std::unique_ptr<Stmt> parseUniformDecl();
    std::unique_ptr<Stmt> parseTextureDecl(TextureSamplerKind kind);
    // Phase 3.2 Block 3: `storage <name> : structuredbuffer<T>` etc.
    std::unique_ptr<Stmt> parseStorageDecl();
    // Phase 3.3 Block 4: `shared <type> <name>[<size>];` workgroup
    // local memory (compute body only; ignored outside compute).
    std::unique_ptr<Stmt> parseSharedDecl();
    // Phase 3.4: `uniformblock <Name> { <type> <field>; ... }` top-level
    // uniform buffer object (GLSL UBO with std140 layout + binding slot).
    std::unique_ptr<Stmt> parseUniformBlockDecl();
    std::unique_ptr<Stmt> parseVertexFunc();
    std::unique_ptr<Stmt> parseFragmentFunc();
    std::unique_ptr<Stmt> parseShaderParam(ShaderParam::Direction dir);
    std::unique_ptr<Stmt> parseShaderBlockBody(std::vector<StmtPtr>& params, bool allowOut);
    std::unique_ptr<Stmt> parseVariantAttribute();

    std::unique_ptr<Stmt> parseLetStmt();
    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseIfStmt();
    std::unique_ptr<Stmt> parseForStmt();

    // Helper methods
    const Token& current() const;
    const Token& previous() const;
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool match(std::initializer_list<TokenType> types);
    Token advance();
    bool isAtEnd() const;
    void error(const std::string& message);
    Token consume(TokenType type, const std::string& message);

    // Phase 2 Step 4: panic-mode recovery. After a parse error inside a
    // material block or expression, advance tokens until we land on a
    // synchronizing token (top-level statement keyword, '}' closing the
    // current block, or EOF). This lets the parser continue parsing the
    // NEXT material / declaration instead of cascading every subsequent
    // token into another error.
    //
    // Boundary tokens for top-level: material / property / uniform /
    // texture2d / vertex / fragment / left-brace / left-bracket / EOF.
    // Boundary tokens for shader block: '}' / EOF.
    void synchronize();
    // Variant of synchronize for use inside vertex/fragment bodies —
    // stops at the next '}' that closes the current block, or EOF.
    void synchronizeToBlockEnd();

    // Phase 1: consume either an Identifier or any of the builtin type
    // keywords (Float/Vec2/.../Bool). Phase 2 demotes type keywords to
    // Identifier (design.md §11.1) and this helper collapses back to a
    // plain consume(Identifier, ...).
    Token consumeTypeName(const std::string& message);

    // Phase 1: consume an Identifier OR any Phoskia semantic / io keyword
    // (position / normal / color / texcoord / in / out) as a name. This
    // is needed because those tokens are reserved at the lexer level but
    // frequently re-used as identifier names (e.g. `property color = ...`).
    Token consumeName(const std::string& message);

    int getPrecedence(TokenType op);

    std::vector<Token> _tokens;
    int _current = 0;
    CompilerErrorReporter _reporter;
};

} // namespace ayt::shader::phoskia
