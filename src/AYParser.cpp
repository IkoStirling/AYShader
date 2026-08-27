// AYParser.cpp - Parser implementation

#include "AYShader/Parser.h"
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>

namespace ayt::shader::phoskia
{

// ----- Audit 2026-08-26 P-M-01..04: forward declarations for the
// robust integer literal parsers. They're defined further down the
// file (after Parser's other method implementations). Putting the
// declarations at the top of the file means the binding / array-size
// call sites in parseUniformDecl / parseSharedDecl / parseStorageDecl
// / parseUniformBlockDecl can reference them without needing the
// inline definitions to be lexically prior to the use site.
enum class IntParseFailure;
struct IntParseResult;
static int parseNonNegativeInt(const std::string& lexeme, int maxInclusive,
        std::function<void(const std::string&)> reporter,
        const std::string& fieldLabel, bool& failed);
static int parsePositiveInt(const std::string& lexeme, int maxInclusive,
        std::function<void(const std::string&)> reporter,
        const std::string& fieldLabel, bool& failed);

static const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::Material: return "Material";
        case TokenType::Property: return "Property";
        case TokenType::Uniform: return "Uniform";
        case TokenType::Texture2D: return "Texture2D";
        case TokenType::TextureCube: return "TextureCube";
        case TokenType::Sampler: return "Sampler";
        case TokenType::Vertex: return "Vertex";
        case TokenType::Fragment: return "Fragment";
        case TokenType::Compute: return "Compute";
        case TokenType::Let: return "Let";
        case TokenType::If: return "If";
        case TokenType::Else: return "Else";
        case TokenType::For: return "For";
        case TokenType::In: return "In";
        case TokenType::Out: return "Out";
        case TokenType::Return: return "Return";
        case TokenType::True: return "True";
        case TokenType::False: return "False";
        case TokenType::Variant: return "Variant";
        case TokenType::Position: return "Position";
        case TokenType::Normal: return "Normal";
        case TokenType::Color: return "Color";
        case TokenType::Texcoord: return "Texcoord";
        case TokenType::Plus: return "Plus";
        case TokenType::Minus: return "Minus";
        case TokenType::Star: return "Star";
        case TokenType::Slash: return "Slash";
        case TokenType::Percent: return "Percent";
        case TokenType::Equal: return "Equal";
        case TokenType::EqualEqual: return "EqualEqual";
        case TokenType::Bang: return "Bang";
        case TokenType::BangEqual: return "BangEqual";
        case TokenType::Less: return "Less";
        case TokenType::LessEqual: return "LessEqual";
        case TokenType::Greater: return "Greater";
        case TokenType::GreaterEqual: return "GreaterEqual";
        case TokenType::And: return "And";
        case TokenType::Or: return "Or";
        case TokenType::Dot: return "Dot";
        case TokenType::Comma: return "Comma";
        case TokenType::Colon: return "Colon";
        case TokenType::Semicolon: return "Semicolon";
        case TokenType::LeftParen: return "LeftParen";
        case TokenType::RightParen: return "RightParen";
        case TokenType::LeftBrace: return "LeftBrace";
        case TokenType::RightBrace: return "RightBrace";
        case TokenType::LeftBracket: return "LeftBracket";
        case TokenType::RightBracket: return "RightBracket";
        case TokenType::Identifier: return "Identifier";
        case TokenType::FloatLiteral: return "FloatLiteral";
        case TokenType::IntLiteral: return "IntLiteral";
        case TokenType::StringLiteral: return "StringLiteral";
        case TokenType::EndOfFile: return "EndOfFile";
        case TokenType::Unknown: return "Unknown";
        case TokenType::Binding: return "Binding";   // Phase 3.5-A
    }
    return "?";
}

Parser::Parser(const std::vector<Token>& tokens)
    : _tokens(tokens), _current(0) {}

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>(std::vector<StmtPtr>{});
    while (!isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            program->declarations.push_back(std::move(stmt));
        } else {
            // Phase 2 Step 4: panic-mode recovery. Instead of advancing
            // a single token (which leaves the parser stuck on whatever
            // garbage token caused the original error), skip forward to
            // the next top-level statement boundary. This lets the
            // parser keep parsing any subsequent valid material /
            // property / uniform / etc. after the first error.
            synchronize();
        }
        if (_current == before) {
            // Defense-in-depth: if synchronize() somehow didn't move
            // (e.g. malformed input with no boundary), force a single
            // advance so the main loop terminates.
            std::cerr << "[parse] STUCK at _current=" << _current
                      << " curType=" << tokenTypeName(current().type)
                      << " lexeme='" << current().lexeme << "' — force advancing\n";
            advance();
        }
    }
    return program;
}

std::unique_ptr<Stmt> Parser::parseStatement() {
    if (match(TokenType::Material)) {
        return parseMaterialDecl();
    }
    // Phase 3.3 Block 2: dispatch to parseComputeDeclWithAttributes
    // when the current token is `[` AND the bracket contents look like
    // a compute attribute (`[numthreads(...)]`). A bare `[` could also
    // be a `[variant name]` inside a material body — those go through
    // the regular parseShaderBlockBody path, NOT this dispatch.
    //
    // To distinguish them without consuming: peek the next token. If
    // it's the identifier "numthreads", it's a compute attribute; the
    // material-body variant attribute uses the keyword `variant`.
    // (See parseVariantAttribute.) The two are disjoint lexemes today.
    if (check(TokenType::LeftBracket) && _current + 1 < _tokens.size() &&
        _tokens[_current + 1].type == TokenType::Identifier &&
        _tokens[_current + 1].lexeme == "numthreads") {
        std::array<uint32_t, 3> numThreads{};
        bool hasNumThreads = false;
        auto decl = parseComputeDeclWithAttributes(numThreads, hasNumThreads);
        if (decl) {
            decl->hasNumThreads = hasNumThreads;
            decl->numThreads = numThreads;
            return decl;
        }
        // The attribute wasn't a recognised numthreads form (parser
        // errors already reported). Return null so the outer loop
        // synchronises.
        return nullptr;
    }
    if (match(TokenType::Compute)) {
        // No-attribute compute decl. parseComputeDeclWithAttributes
        // still does the work; its `[...]` loop is a no-op when
        // current token is an identifier (the name).
        std::array<uint32_t, 3> numThreads{};
        bool hasNumThreads = false;
        auto decl = parseComputeDeclWithAttributes(numThreads, hasNumThreads);
        if (decl) {
            decl->hasNumThreads = hasNumThreads;
            decl->numThreads = numThreads;
        }
        return decl;
    }
    if (match(TokenType::Property)) {
        return parsePropertyDecl();
    }
    if (match(TokenType::Uniform)) {
        return parseUniformDecl();
    }
    if (match(TokenType::Storage)) {
        // Phase 3.2 Block 3: compute storage buffer declaration.
        // `storage <name> : structuredbuffer<T>` / `... : rwstructuredbuffer<T>`
        // are only meaningful inside a compute body, but the parser
        // doesn't enforce that — it just emits the AST node. The
        // IRGenerator / BGFX converter accept storage declarations
        // anywhere they see them (and Phase 3.2 ignores them outside
        // compute bodies anyway — only convertComputeDecl emits them).
        return parseStorageDecl();
    }
    if (match(TokenType::Shared)) {
        // Phase 3.3 Block 4: workgroup-shared local memory. Same
        // scope-loose policy as `storage` — the parser emits the AST
        // node and only convertComputeDecl turns it into GLSL.
        return parseSharedDecl();
    }
    if (match(TokenType::UniformBlock)) {
        // Phase 3.4: top-level uniform buffer object. The dispatcher
        // here catches `uniformblock` anywhere (including inside a
        // material body by mistake); the parser doesn't enforce
        // top-level-only placement, but the IRGenerator handles UBO
        // decls only at the program-root scope — anything inside a
        // material body silently lowers to nothing (no code path
        // walks it). Tests that want UBO at the program root simply
        // place it next to material / compute.
        return parseUniformBlockDecl();
    }
    if (match(TokenType::Texture2D)) {
        return parseTextureDecl(TextureSamplerKind::Sampler2D);
    }
    if (match(TokenType::TextureCube)) {
        return parseTextureDecl(TextureSamplerKind::SamplerCube);
    }
    if (match(TokenType::Vertex)) {
        // IR-H-01: snapshot the `vertex` keyword's source location so
        // VertexFunc carries the source line of the shader-block open.
        Token vTok = previous();
        auto vf = parseVertexFunc();
        if (vf) {
            if (auto* vfp = dynamic_cast<VertexFunc*>(vf.get())) {
                vfp->line = vTok.line;
                vfp->column = vTok.column;
            }
        }
        return vf;
    }
    if (match(TokenType::Fragment)) {
        // IR-H-01: mirror vertex — stamp FragmentFunc with the `fragment`
        // keyword's location so shader-block-scoped errors carry source info.
        Token fTok = previous();
        auto ff = parseFragmentFunc();
        if (ff) {
            if (auto* ffp = dynamic_cast<FragmentFunc*>(ff.get())) {
                ffp->line = fTok.line;
                ffp->column = fTok.column;
            }
        }
        return ff;
    }
    if (match(TokenType::Let)) {
        return parseLetStmt();
    }
    if (match(TokenType::Return)) {
        return parseReturnStmt();
    }
    if (match(TokenType::If)) {
        return parseIfStmt();
    }
    if (match(TokenType::For)) {
        return parseForStmt();
    }
    if (match(TokenType::LeftBracket)) {
        return parseVariantAttribute();
    }
    // Bare expression at statement level — wrap as ExprStmt
    auto expr = parseExpression();
    return std::make_unique<ExprStmt>(std::move(expr));
}

std::unique_ptr<Expr> Parser::parseExpression() {
    return parseBinary();
}

std::unique_ptr<Expr> Parser::parseBinary(int precedence) {
    auto left = parseUnary();

    while (!isAtEnd()) {
        TokenType op = current().type;
        int nextPrecedence = getPrecedence(op);
        // '=' is right-associative and lowest precedence (1). We use strict <
        // (not <=) so the recursive parseBinary(nextPrec) on the right side
        // also consumes another '=' at the same precedence, building nested
        // assignments `a = (b = c)`. For left-assoc ops (`+`, `*`, etc.) the
        // recursion consumes only higher-precedence ops and naturally stops.
        if (nextPrecedence < precedence) break;
        if (nextPrecedence == 0) break;

        // Capture the operator token BEFORE advancing. The recursive
        // parseBinary(nextPrecedence) will itself advance through parsePrimary
        // at least once, so `previous()` after the recursion no longer points
        // at the operator — it points at the operand that followed. We must
        // snapshot the operator here while `_current` is still on it.
        Token opTok = current();
        advance();
        auto right = parseBinary(nextPrecedence);
        // IR-H-01 / IR-M-01: stamp the binary operator's source line/column
        // so the analyzer's matrix-mismatch / type-error diagnostics can
        // point at the operator location rather than the program origin.
        auto binExpr = std::make_unique<BinaryExpr>(std::move(left), opTok, std::move(right));
        binExpr->line = opTok.line;
        binExpr->column = opTok.column;
        left = std::move(binExpr);
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseUnary() {
    if (match(TokenType::Minus) || match(TokenType::Bang)) {
        Token op = previous();
        auto operand = parseUnary();
        return std::make_unique<UnaryExpr>(op, std::move(operand));
    }
    return parseCall();
}

std::unique_ptr<Expr> Parser::parseCall() {
    auto expr = parsePrimary();

    while (true) {
        if (match(TokenType::LeftParen)) {
            std::vector<ExprPtr> args;
            if (!check(TokenType::RightParen)) {
                do {
                    args.push_back(parseExpression());
                } while (match(TokenType::Comma));
            }
            consume(TokenType::RightParen, "Expected ')' after arguments");
            // IR-H-03: stamp the call site at the LEFT-paren location so
            // arity-mismatch / unknown-callable diagnostics point at the
            // callee's `(` rather than the program origin.
            Token lparen = previous();
            auto callExpr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
            callExpr->line = lparen.line;
            callExpr->column = lparen.column;
            expr = std::move(callExpr);
        } else if (match(TokenType::Dot)) {
            Token name = consumeName("Expected property name after '.'");
            // IR-M-02: stamp the member-access location for out-of-range
            // swizzle / unknown-member diagnostics.
            auto memExpr = std::make_unique<MemberExpr>(std::move(expr), name.lexeme);
            memExpr->line = name.line;
            memExpr->column = name.column;
            expr = std::move(memExpr);
        } else if (check(TokenType::LeftBracket)) {
            // Lookahead: a `[` immediately followed by `variant` is the
            // start of a `[variant name]` attribute, NOT array indexing.
            // parseCall bails out so the enclosing parseStatement can
            // re-dispatch on LeftBracket → parseVariantAttribute on the
            // next iteration. This matches Phoskia's grammar where
            // attributes are statement-level, not expression-level.
            if (_current + 1 < _tokens.size() &&
                _tokens[_current + 1].type == TokenType::Variant) {
                break;
            }
            advance();  // consume `[`
            auto index = parseExpression();
            consume(TokenType::RightBracket, "Expected ']' after index");
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
    if (match(TokenType::FloatLiteral)) {
        Token token = previous();
        try {
            return std::make_unique<LiteralExpr>(std::stof(token.lexeme));
        } catch (const std::exception&) {
            return std::make_unique<LiteralExpr>(0.0f);
        }
    }
    if (match(TokenType::IntLiteral)) {
        Token token = previous();
        try {
            return std::make_unique<LiteralExpr>(static_cast<int>(std::stol(token.lexeme)));
        } catch (const std::exception&) {
            return std::make_unique<LiteralExpr>(0);
        }
    }
    if (match(TokenType::StringLiteral)) {
        Token token = previous();
        return std::make_unique<LiteralExpr>(token.lexeme);
    }
    if (match(TokenType::True)) {
        return std::make_unique<LiteralExpr>(true);
    }
    if (match(TokenType::False)) {
        return std::make_unique<LiteralExpr>(false);
    }
    if (match(TokenType::Identifier)) {
        return std::make_unique<IdentifierExpr>(previous().lexeme);
    }
    // Phoskia semantic keywords (position / normal / color / texcoord) and
    // the io keywords (in / out) are also valid identifiers in expression
    // context — e.g. `gl_FragColor * color`, `out = normal`. Builtin type
    // names like vec3 / float / mat4 are now plain Identifiers too (see
    // AYShader\Token.h / design.md §11.1 — "类型名降级重构"), so the previous
    // 13-branch workaround for the legacy type keywords has been
    // removed.
    if (match(TokenType::Position) || match(TokenType::Normal) ||
        match(TokenType::Color)    || match(TokenType::Texcoord) ||
        match(TokenType::In)       || match(TokenType::Out)) {
        return std::make_unique<IdentifierExpr>(previous().lexeme);
    }
    if (match(TokenType::LeftParen)) {
        auto expr = parseExpression();
        consume(TokenType::RightParen, "Expected ')' after expression");
        return expr;
    }

    std::cerr << "[parsePrimary] FAILED — no match at _current=" << _current
              << " curType=" << tokenTypeName(current().type) << "\n";
    error("Expected expression");
    return nullptr;
}

std::unique_ptr<Stmt> Parser::parseMaterialDecl() {
    Token name = consumeName("Expected material name");
    consume(TokenType::LeftBrace, "Expected '{' before material body");

    std::vector<StmtPtr> declarations;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            declarations.push_back(std::move(stmt));
        } else {
            // Phase 2 Step 4: panic-mode recovery inside the material
            // body. Skip tokens until we land on a token that can start
            // a fresh inner declaration (property / uniform / texture2d /
            // vertex / fragment / let / etc.) or on the closing '}'.
            // This stops one bad declaration from cascading into all
            // subsequent ones.
            synchronize();
        }
        if (_current == before) {
            // Defense-in-depth: force one advance so we don't spin.
            std::cerr << "[parseMaterialDecl.loop] STUCK at _current=" << _current
                      << " curType=" << tokenTypeName(current().type)
                      << " lexeme='" << current().lexeme << "' — force advancing\n";
            advance();
        }
    }

    consume(TokenType::RightBrace, "Expected '}' after material body");

    // Structural validation: every material must declare exactly one
    // vertex block and one fragment block. Phase 1 of the converter
    // (vs/fs three-piece output) has no concept of a "compute-only"
    // material — that lives in Phase 2 alongside the compute backend.
    bool hasVertex = false, hasFragment = false;
    for (const auto& d : declarations) {
        if (dynamic_cast<VertexFunc*>(d.get()))   hasVertex = true;
        if (dynamic_cast<FragmentFunc*>(d.get())) hasFragment = true;
    }
    if (!hasVertex) {
        error("Material '" + name.lexeme + "' is missing a vertex { } block");
    }
    if (!hasFragment) {
        error("Material '" + name.lexeme + "' is missing a fragment { } block");
    }

    return std::make_unique<MaterialDecl>(name.lexeme, std::move(declarations));
}

// Phase 3.3 Block 2: `[numthreads(X, Y, Z)] compute Foo { ... }`
// attribute. Lets the user pin the workgroup shape per-declaration
// instead of inheriting the BGFX backend's hardcoded 64 default.
//
// Syntax: an optional attribute list before `compute Name`, where each
// attribute is `[numthreads(<int_literal>, <int_literal>, <int_literal>)]`.
// Phoskia only exposes `numthreads` today; future attributes (`[local_size(...)]`,
// `[max_registers(...)]`) would slot in here.
//
// The attribute is consumed by parseComputeDecl-with-attribute and
// stored on the resulting ComputeDecl node. The IRGenerator carries it
// forward to IRComputeDecl::numThreads; the BGFX backend emits the
// GLSL `layout(local_size_x = N, local_size_y = N, local_size_z = N) in;`
// directive with the user's values.

std::unique_ptr<ComputeDecl> Parser::parseComputeDeclWithAttributes(
    std::array<uint32_t, 3>& outNumThreads, bool& outHasNumThreads) {
    // Optional attribute list — at most one `[numthreads(X, Y, Z)]` for
    // now. We allow other attributes to be skipped silently so a future
    // `[max_registers(...)]` doesn't break older Phoskia sources.
    while (check(TokenType::LeftBracket)) {
        advance();  // consume '['
        Token attrName = consume(TokenType::Identifier, "Expected attribute name in [...]");
        if (attrName.lexeme == "numthreads") {
            consume(TokenType::LeftParen, "Expected '(' after 'numthreads'");
            // Three int literals: X, Y, Z. GLSL accepts `local_size_y = 1`
            // and `local_size_z = 1` defaults; we require all three to
            // match the user-written form byte-for-byte (Phoskia doesn't
            // try to be clever about omitted dimensions).
            Token xTok = consume(TokenType::IntLiteral, "Expected int literal for numthreads X");
            consume(TokenType::Comma, "Expected ',' after numthreads X");
            Token yTok = consume(TokenType::IntLiteral, "Expected int literal for numthreads Y");
            consume(TokenType::Comma, "Expected ',' after numthreads Y");
            Token zTok = consume(TokenType::IntLiteral, "Expected int literal for numthreads Z");
            consume(TokenType::RightParen, "Expected ')' after numthreads Z");
            bool failed = false;
            bool componentFailed = false;
            outNumThreads[0] = static_cast<uint32_t>(parsePositiveInt(
                xTok.lexeme, std::numeric_limits<int>::max(),
                [this](const std::string& m) { error(m); },
                "numthreads X", componentFailed));
            failed = failed || componentFailed;
            outNumThreads[1] = static_cast<uint32_t>(parsePositiveInt(
                yTok.lexeme, std::numeric_limits<int>::max(),
                [this](const std::string& m) { error(m); },
                "numthreads Y", componentFailed));
            failed = failed || componentFailed;
            outNumThreads[2] = static_cast<uint32_t>(parsePositiveInt(
                zTok.lexeme, std::numeric_limits<int>::max(),
                [this](const std::string& m) { error(m); },
                "numthreads Z", componentFailed));
            failed = failed || componentFailed;
            if (failed) {
                return nullptr;
            }
            outHasNumThreads = true;
        } else {
            // Unknown attribute — skip the bracketed expression. We
            // consume the closing ']' and move on. A more strict parser
            // would reject unknown attributes; Phase 3.3 is permissive
            // so future attributes don't break older sources.
            skipBracketedAttributeBody();
        }
        consume(TokenType::RightBracket, "Expected ']' after compute attribute");
    }

    // After the attribute loop we may or may not have consumed the
    // `compute` keyword yet. The dispatcher in parseStatement handles
    // both shapes: when it sees `[numthreads`, it routes here without
    // consuming `compute` (because the helper eats the `[...]` first);
    // when it sees a bare `compute`, it consumes the keyword first
    // and then routes here with the attribute loop being a no-op.
    // To keep the helper self-contained we optionally consume `compute`
    // here — match() doesn't error if the token is wrong.
    match(TokenType::Compute);  // optional — present iff dispatcher didn't pre-consume it
    Token name = consumeName("Expected compute name");
    consume(TokenType::LeftBrace, "Expected '{' before compute body");

    std::vector<StmtPtr> body;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            if (stmt) body.push_back(std::move(stmt));
        } else {
            synchronize();
        }
        if (_current == before) {
            advance();
        }
    }

    consume(TokenType::RightBrace, "Expected '}' after compute body");
    return std::make_unique<ComputeDecl>(name.lexeme, std::move(body));
}

// Helper: skip the inner contents of an unknown `[name ...]` attribute.
// Used when we encounter an attribute we don't recognise so the parser
// can continue with the next token. The closing ']' is consumed by the
// caller.
void Parser::skipBracketedAttributeBody() {
    // We track depth for BOTH parens AND brackets. The original
    // implementation only counted parens, which is sufficient for the
    // current attribute grammar (numthreads's args are nested only
    // through `()`). But a future attribute like `[foo([...])]` would
    // have nested `[...]` and the old code would stop at the FIRST
    // `]` it found, dropping content. Audit 2026-08-26 P-H-01
    // promoted this to a High-severity correctness fix: the helper
    // must skip over any nested bracketed sub-expression and only
    // return when we see the `]` that closes the attribute at depth
    // zero.
    //
    // Note: the closing ']' of the OUTERMOST attribute is consumed by
    // the caller (parseComputeDeclWithAttributes via
    // `consume(TokenType::RightBracket, ...)`), so we stop at the
    // first `]` that brings our bracket depth back to zero. That is
    // precisely the inner closing `]` of the outermost attribute's
    // level-0 entry, and the caller pairs it with the opening `[` it
    // already consumed.
    int parenDepth = 0;
    int bracketDepth = 0;
    while (!isAtEnd()) {
        if (parenDepth == 0 && bracketDepth == 0 &&
            check(TokenType::RightBracket)) {
            return;
        }
        if (check(TokenType::LeftParen)) {
            ++parenDepth;
            advance();
        } else if (check(TokenType::RightParen)) {
            // Tolerate unbalanced `)` (best-effort recovery). Only
            // decrement if we previously saw a matching `(`.
            if (parenDepth > 0) --parenDepth;
            advance();
        } else if (check(TokenType::LeftBracket)) {
            ++bracketDepth;
            advance();
        } else if (check(TokenType::RightBracket)) {
            // Tolerate unbalanced `]`. Only decrement if we previously
            // saw a matching `[`.
            if (bracketDepth > 0) --bracketDepth;
            // If bracketDepth just dropped to zero, the OUTER loop
            // guard at the top of the next iteration handles it
            // correctly — we just stop on the next iteration.
            advance();
        } else {
            advance();
        }
    }
}

std::unique_ptr<Stmt> Parser::parsePropertyDecl() {
    Token name = consumeName("Expected property name");
    consume(TokenType::Equal, "Expected '=' after property name");
    auto initializer = parseExpression();
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<PropertyDecl>(name.lexeme, std::move(initializer));
}

std::unique_ptr<Stmt> Parser::parseUniformDecl() {
    // Phase 1: also accept builtin type keywords (Float/Vec2/.../Bool) since
    // Lexer emits them as dedicated tokens. Phase 2's "demote to Identifier"
    // refactor (design.md §11.1) will let us revert to a single Identifier
    // consume() once the Lexer change lands.
    Token type = consumeTypeName("Expected uniform type");
    Token name = consumeName("Expected uniform name");
    // Fixed-size array suffix — same grammar as UniformBlockField:
    //   uniform mat4 lightViewProjs[8]
    // Without this, `[8]` is left for the next statement and indexing
    // type-inference treats the uniform as a bare mat4 → typeless
    // `let` emit (`_lvp0 = ...` → HLSL X3004 undeclared).
    int arrayLength = 0;
    if (match(TokenType::LeftBracket)) {
        Token sizeTok = consume(TokenType::IntLiteral,
                                "Expected integer literal for uniform array size");
        // Audit 2026-08-26 P-M-01..P-M-02: routed through the
        // parsePositiveInt helper so `0xFF` and other base-prefixed
        // literals work, and so an out-of-range value is reported
        // with a clear bound (vs the silent truncation the pre-fix
        // std::stol path produced). We use INT16_MAX as the upper
        // bound; anything larger is a nonsense uniform array.
        bool failed = false;
        arrayLength = parsePositiveInt(sizeTok.lexeme,
                                       std::numeric_limits<int>::max(),
                                       [this](const std::string& m) { error(m); },
                                       "uniform array size",
                                       failed);
        if (failed) return nullptr;
        consume(TokenType::RightBracket,
                "Expected ']' after uniform array size");
    }
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<UniformDecl>(type.lexeme, name.lexeme, arrayLength);
}

std::unique_ptr<Stmt> Parser::parseTextureDecl(TextureSamplerKind kind) {
    Token name = consumeName("Expected texture name");
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<TextureDecl>(name.lexeme, kind);
}

// Phase 3.2 Block 3: parse a storage buffer declaration.
//
//   storage <name> : structuredbuffer<T>     -> read access
//   storage <name> : rwstructuredbuffer<T>   -> read-write access
//
// The element type `T` is captured as a GLSL lexeme; the parser only
// accepts builtin scalar / vector forms (float / int / vec2..4 /
// ivec2..4 / etc). We don't validate the lexeme here — the semantic
// analyzer + IRGenerator's `typeFromGLSLLexeme` path covers that; an
// unknown lexeme falls through as a warning ("could not infer element
// type; GLSL emission will use vec4 fallback"). Custom struct types
// are a Phase 3.3 extension.
std::unique_ptr<Stmt> Parser::parseStorageDecl() {
    Token name = consumeName("Expected storage buffer name");
    consume(TokenType::Colon, "Expected ':' after storage buffer name");

    // The access keyword — `structuredbuffer` (read) or
    // `rwstructuredbuffer` (read-write). Both lower to the same GLSL
    // `buffer` block; the access field is preserved in IR for HLSL
    // emitter use (Phase 5+).
    Token accessKw = consume(TokenType::Identifier, "Expected 'structuredbuffer' or 'rwstructuredbuffer'");
    StorageDecl::Access access;
    if (accessKw.lexeme == "structuredbuffer") {
        access = StorageDecl::Access::Read;
    } else if (accessKw.lexeme == "rwstructuredbuffer") {
        access = StorageDecl::Access::ReadWrite;
    } else {
        error("Expected 'structuredbuffer' or 'rwstructuredbuffer', got '" +
              accessKw.lexeme + "'");
        return nullptr;
    }

    // Optional `<T>` element type. We require it — there's no
    // meaningful default for storage buffer element type (it would
    // either be `vec4` (almost certainly wrong) or invalid in GLSL).
    consume(TokenType::Less, "Expected '<' before storage buffer element type");
    Token elementType = consumeName("Expected storage buffer element type name");
    consume(TokenType::Greater, "Expected '>' after storage buffer element type");

    // Audit 2026-08-26 P-H-02: accept an optional trailing ';' between
    // the closing '>' and the `binding` suffix. Users naturally write
    // either form:
    //
    //   storage x : rwstructuredbuffer<vec4> binding 5;   (compact)
    //   storage x : rwstructuredbuffer<vec4>; binding 5;   (mirrors C-style)
    //
    // Without this, the second form would consume `;` first, then see a
    // free `binding` keyword at the top level and fail to pair it with
    // this declaration. We eat any semicolons defensively here so the
    // match(Binding) below sees the binding keyword instead.
    while (match(TokenType::Semicolon)) { /* drain stray terminators */ }

    // Phase 3.5-A: optional `binding <int>` suffix. When the user
    // writes `binding N;`, the parser records N as an explicit GLSL
    // binding slot; the BGFX backend emits
    // `layout(std430, binding = N)` and validates uniqueness at
    // compile time. Absence (-1) preserves the Phase 3.2-3.4
    // auto-assign behaviour (the BGFX backend assigns sequential
    // slots starting from 0, skipping any user-declared ones).
    int binding = -1;
    if (match(TokenType::Binding)) {
        Token bTok = consume(TokenType::IntLiteral,
            "Expected integer literal after 'binding'");
        // Audit 2026-08-26 P-M-01..P-M-02: route through parseNonNegativeInt.
        // GLSL `binding = N` accepts any 32-bit non-negative value;
        // we clamp the upper bound at INT16_MAX for sanity but the
        // range is generous.
        bool bindFailed = false;
        binding = parseNonNegativeInt(bTok.lexeme,
                                      65535,  // GL_MAX_*_BINDINGS in practice is < 32k
                                      [this](const std::string& m) { error(m); },
                                      "storage binding",
                                      bindFailed);
        if (bindFailed) return nullptr;
    }

    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<StorageDecl>(access, name.lexeme,
                                         elementType.lexeme, binding);
}

// Phase 3.3 Block 4: parse a workgroup-shared local-memory declaration.
//
//   shared <type> <name>[<size>];
//
// Element type is a builtin scalar / vector GLSL lexeme (float / int /
// uint / vec3 / etc.). The size must be a positive int literal —
// workgroup memory is statically sized at compile time; a runtime
// size would either need a `let shared_arr_size = ...` indirection
// (not supported) or a const-expression (out of scope for Phase 3.3).
//
// The parser does not validate the element-type lexeme — the
// IRGenerator's `lexemeToType` table covers builtin forms and falls
// back with a warning for anything else (then the BGFX converter
// defaults to `vec4` for the emit, just like StorageDecl's unknown
// element type).
std::unique_ptr<Stmt> Parser::parseSharedDecl() {
    Token elementType = consumeName("Expected shared element type");
    Token name = consumeName("Expected shared array name");
    consume(TokenType::LeftBracket, "Expected '[' before shared array size");
    Token sizeTok = consume(TokenType::IntLiteral, "Expected integer literal for shared array size");
    consume(TokenType::RightBracket, "Expected ']' after shared array size");
    int size = 0;
    // Audit 2026-08-26 P-M-01..P-M-02: routed through parsePositiveInt
    // so `0x40` and friends parse correctly and out-of-range literals
    // report a clean diagnostic.
    bool sizeFailed = false;
    size = parsePositiveInt(sizeTok.lexeme,
                            std::numeric_limits<int>::max(),
                            [this](const std::string& m) { error(m); },
                            "shared array size",
                            sizeFailed);
    if (sizeFailed) return nullptr;
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<SharedDecl>(elementType.lexeme, name.lexeme, size);
}

// Phase 3.4: parse a top-level uniform buffer object (GLSL UBO).
//
//   uniformblock Camera {
//       vec3 position
//       float fov
//   }
//
// The block name doubles as the instance name (GLSL convention):
// inside shader bodies, fields are accessed as `Camera.position` (a
// regular MemberExpr). Each field is a bare `<type> <name>` line
// (the `uniform` keyword prefix is not repeated — it's implicit
// because we're inside a uniform block). Trailing `;` on each field
// is optional (Phoskia Python-like convention).
//
// The parser doesn't enforce top-level-only placement; UBO is
// semantically a program-scope construct. The IRGenerator handles
// UBO decls only at the program-root scope — anything inside a
// material body silently lowers to nothing (no code path walks it).
std::unique_ptr<Stmt> Parser::parseUniformBlockDecl() {
    Token name = consumeName("Expected uniform block name");
    consume(TokenType::LeftBrace, "Expected '{' before uniform block body");
    std::vector<UniformBlockField> fields;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        // Each field: <type> <name> [<int_literal>]? ';' (optional)
        // The `type` token must be an Identifier (covers builtin
        // scalar / vector names that were demoted from keywords in
        // Phase 2 Step 5 — see design.md §11.1).
        //
        // Phase 1 RD-04: optional fixed-size array suffix.
        //   mat4 bones[128];
        // The literal int inside `[...]` is consumed as `arrayLength`.
        // Std140 layout expands to N*elementSize bytes; GLSL emit
        // produces `mat4 bones[128];`.
        Token fieldType = consumeName("Expected uniform block field type");
        Token fieldName = consumeName("Expected uniform block field name");
        int arrayLength = 0;
        if (match(TokenType::LeftBracket)) {
            Token sizeTok = consume(TokenType::IntLiteral,
                                    "Expected integer literal for array size");
            // Audit 2026-08-26 P-M-01..P-M-02: route through parsePositiveInt.
            // std140 layout caps at std140-practical limits; we
            // accept the full int range here and the IR layer
            // applies std140 validation later.
            bool fieldSizeFailed = false;
            arrayLength = parsePositiveInt(sizeTok.lexeme,
                                           std::numeric_limits<int>::max(),
                                           [this](const std::string& m) { error(m); },
                                           "uniform block array size",
                                           fieldSizeFailed);
            if (fieldSizeFailed) return nullptr;
            consume(TokenType::RightBracket,
                    "Expected ']' after array size");
        }
        fields.push_back({fieldType.lexeme, fieldName.lexeme, arrayLength});
        match(TokenType::Semicolon);  // ';' is optional (Python-like)
    }
    consume(TokenType::RightBrace, "Expected '}' after uniform block body");

    // Audit 2026-08-26 P-H-03: accept an optional trailing ';' between
    // the closing '}' and the `binding` suffix. Users naturally write
    // either form:
    //
    //   uniformblock Camera { ... } binding 5;   (compact)
    //   uniformblock Camera { ... }; binding 5;   (mirrors C-style struct)
    //
    // The pre-fix code unconditionally called match(Semicolon) BEFORE
    // match(Binding), so the second form would silently eat the
    // trailing ';' and leave `binding` as a stranded identifier at
    // the top level. The fix: defer the ';' consumption until AFTER
    // we've handled (or skipped) the binding suffix. The match(Binding)
    // path then has a chance to pair itself with this declaration even
    // when the user put a ';' between them.
    //
    // (We allow unbounded trailing semicolons here for robustness —
    // `uniformblock X {};;;` is a stylistic atrocity but legally
    // equivalent. Drain any leading semicolons BEFORE checking for
    // `binding` so that the `binding` keyword is the one we actually
    // see. The remaining `;` after `binding N` is consumed inside the
    // match(Binding) block below.)
    while (match(TokenType::Semicolon)) { /* drain stray terminators */ }

    // Phase 3.5-B: optional `binding <int>;` suffix.
    //
    //   uniformblock Camera { vec3 pos; float fov; } binding 5;
    //
    // Mirrors parseStorageDecl's binding block (lines 567-585): the
    // literal integer is propagated verbatim into the AST, and the
    // BGFX backend either uses it directly or auto-assigns a slot
    // starting from max(explicit) + 1.
    int binding = -1;
    if (match(TokenType::Binding)) {
        Token bTok = consume(TokenType::IntLiteral,
                             "Expected integer literal after 'binding'");
        // Audit 2026-08-26 P-M-01..P-M-02: route through parseNonNegativeInt
        // for consistency with parseStorageDecl's binding block.
        bool bindFailed = false;
        binding = parseNonNegativeInt(bTok.lexeme,
                                      65535,
                                      [this](const std::string& m) { error(m); },
                                      "uniformblock binding",
                                      bindFailed);
        if (bindFailed) return nullptr;
        match(TokenType::Semicolon);  // optional trailing ';'
    }
    // Audit 2026-08-26 P-H-03 (continued): if there was no `binding`
    // suffix but the user wrote a trailing ';' (or `;;;` for some
    // reason), drain those here so the outer parseStatement loop
    // doesn't see them as the start of a fresh top-level statement.
    // Bound with the matching drain above (before the match(Binding))
    // so users can write either form interchangeably.
    while (match(TokenType::Semicolon)) { /* drain stray terminators */ }
    return std::make_unique<UniformBlockDecl>(name.lexeme,
                                              std::move(fields), binding);
}

std::unique_ptr<Stmt> Parser::parseShaderParam(ShaderParam::Direction dir) {
    // We've already consumed 'in' or 'out'. Accept the two forms:
    //   in/out <name>           : <semantic> [= <default>]
    //   in/out <type> <name>    : <semantic> [= <default>]
    // The type prefix is optional — Phoskia keeps the type implicit when
    // it can be derived from the semantic (position → vec3, color →
    // vec4, etc.). Tests use both forms; the converter picks the
    // emitted bgfx type from the semantic regardless of which form was
    // written.

    // Phase 2 Step 5: the previous "peek at builtin type keyword"
    // branch has been removed because builtin type names are now plain
    // Identifier tokens (see AYShader\Token.h / design.md §11.1). The
    // converter maps PhoskiaSemantic → bgfx type, so an optional
    // explicit type prefix in the source is redundant — we simply
    // always treat the next identifier as the parameter name.

    Token name = consumeName("Expected parameter name");
    consume(TokenType::Colon, "Expected ':' after parameter name");

    PhoskiaSemantic semantic;
    if (match(TokenType::Position))      semantic = PhoskiaSemantic::Position;
    else if (match(TokenType::Normal))   semantic = PhoskiaSemantic::Normal;
    else if (match(TokenType::Color))    semantic = PhoskiaSemantic::Color;
    else if (match(TokenType::Texcoord)) semantic = PhoskiaSemantic::Texcoord;
    else if (match(TokenType::BoneIndices)) semantic = PhoskiaSemantic::BoneIndices;
    else if (match(TokenType::BoneWeights)) semantic = PhoskiaSemantic::BoneWeights;
    else if (match(TokenType::Tangent)) semantic = PhoskiaSemantic::Tangent;
    else {
        error("Expected Phoskia semantic type (position/normal/color/texcoord/tangent/boneindices/boneweights)");
        return nullptr;
    }

    // Default value only allowed on `out` params.
    ExprPtr defaultValue = nullptr;
    if (match(TokenType::Equal)) {
        if (dir != ShaderParam::Direction::Out) {
            error("Default value is only allowed on 'out' parameters");
        }
        defaultValue = parseExpression();
    }

    return std::make_unique<ShaderParam>(dir, name.lexeme, semantic, std::move(defaultValue));
}

std::unique_ptr<Stmt> Parser::parseShaderBlockBody(
        std::vector<StmtPtr>& params,
        bool allowOut) {
    // Helper retained as documentation of the shared pattern between
    // parseVertexFunc / parseFragmentFunc. Not called directly — both
    // shader blocks inline this logic because their output node types
    // (VertexFunc / FragmentFunc) differ.
    (void)params; (void)allowOut;
    return nullptr;
}

std::unique_ptr<Stmt> Parser::parseVertexFunc() {
    consume(TokenType::LeftBrace, "Expected '{' before vertex body");
    std::vector<StmtPtr> params;
    std::vector<StmtPtr> body;
    // Inlined parseShaderBlockBody, specialized for vertex (allowOut=true).
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        if (check(TokenType::In)) {
            advance();
            if (auto p = parseShaderParam(ShaderParam::Direction::In)) {
                params.push_back(std::move(p));
            }
            // Trailing ';' on a param line is optional (Python-like).
            match(TokenType::Semicolon);
            continue;
        }
        if (check(TokenType::Out)) {
            advance();
            if (auto p = parseShaderParam(ShaderParam::Direction::Out)) {
                params.push_back(std::move(p));
            }
            match(TokenType::Semicolon);
            continue;
        }
        break;
    }
    // Skip stray semicolons between the param block and the body (e.g. when
    // the source puts ';' after each in/out declaration).
    while (check(TokenType::Semicolon) && !isAtEnd()) advance();
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            bool isNullExpr = false;
            if (auto* es = dynamic_cast<ExprStmt*>(stmt.get())) {
                if (!es->expr) isNullExpr = true;
            }
            if (isNullExpr) {
                advance();
                continue;
            }
            body.push_back(std::move(stmt));
        } else {
            advance();
        }
        if (_current == before) advance();
    }
    consume(TokenType::RightBrace, "Expected '}' after vertex body");
    return std::make_unique<VertexFunc>(std::move(params), std::move(body));
}

std::unique_ptr<Stmt> Parser::parseFragmentFunc() {
    consume(TokenType::LeftBrace, "Expected '{' before fragment body");
    std::vector<StmtPtr> inputs;
    std::vector<StmtPtr> outputs;
    std::vector<StmtPtr> body;
    // Phase 6 #6: fragment may declare MRT `out` targets (max 8). Declaration
    // order becomes gl_FragData[0..N-1]. Prefer `: color` for each target.
    constexpr size_t kMaxFragmentMrtOutputs = 8;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        if (check(TokenType::In)) {
            advance();
            if (auto p = parseShaderParam(ShaderParam::Direction::In)) {
                inputs.push_back(std::move(p));
            }
            match(TokenType::Semicolon);
            continue;
        }
        if (check(TokenType::Out)) {
            advance();
            if (outputs.size() >= kMaxFragmentMrtOutputs) {
                error("Fragment MRT supports at most 8 'out' targets (gl_FragData[0..7])");
                // Still parse the param so recovery stays on track.
                (void)parseShaderParam(ShaderParam::Direction::Out);
            } else if (auto p = parseShaderParam(ShaderParam::Direction::Out)) {
                outputs.push_back(std::move(p));
            }
            match(TokenType::Semicolon);
            continue;
        }
        break;
    }
    while (check(TokenType::Semicolon) && !isAtEnd()) advance();
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            bool isNullExpr = false;
            if (auto* es = dynamic_cast<ExprStmt*>(stmt.get())) {
                if (!es->expr) isNullExpr = true;
            }
            if (isNullExpr) {
                advance();
                continue;
            }
            body.push_back(std::move(stmt));
        } else {
            advance();
        }
        if (_current == before) advance();
    }
    consume(TokenType::RightBrace, "Expected '}' after fragment body");
    return std::make_unique<FragmentFunc>(
        std::move(inputs), std::move(outputs), std::move(body));
}

std::unique_ptr<Stmt> Parser::parseVariantAttribute() {
    // We've already consumed '[' via match() in parseStatement.
    // Two accepted forms:
    //   [ variant <name> ]   — 'variant' keyword followed by an Identifier
    //   [ <name> ]           — bare Identifier (legacy/shorthand)
    Token name;
    if (match(TokenType::Variant)) {
        // 'variant' keyword — read the next Identifier as the attribute name.
        name = consumeName("Expected variant attribute name after 'variant'");
    } else if (check(TokenType::Identifier)) {
        name = current();
        advance();
    } else {
        error("Expected variant name after '['");
        return nullptr;
    }
    consume(TokenType::RightBracket, "Expected ']' after variant name");
    return std::make_unique<VariantAttribute>(name.lexeme);
}

std::unique_ptr<Stmt> Parser::parseLetStmt() {
    Token name = consumeName("Expected variable name");
    consume(TokenType::Equal, "Expected '=' after let");
    auto initializer = parseExpression();
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    // IR-H-01: stamp the let-binding's source location from the variable
    // name token. The analyzer falls back to its current-location hint
    // when this is 0 (e.g. hand-built AST in unit tests).
    auto letStmt = std::make_unique<LetStmt>(name.lexeme, std::move(initializer));
    letStmt->line = name.line;
    letStmt->column = name.column;
    return std::move(letStmt);
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    // IR-H-02: snapshot the `return` keyword's location BEFORE consuming
    // the optional value expression — once we recurse into parseExpression,
    // _current advances past the value and the keyword location is lost.
    int retLine = previous().line;
    int retCol = previous().column;
    std::unique_ptr<Expr> value = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = parseExpression();
    }
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    auto retStmt = std::make_unique<ReturnStmt>(std::move(value));
    retStmt->line = retLine;
    retStmt->column = retCol;
    return std::move(retStmt);
}

std::unique_ptr<Stmt> Parser::parseIfStmt() {
    // IR-H-04: snapshot the `if` keyword's location before recursing into
    // the condition so we can stamp the IfStmt for source-location-aware
    // diagnostics.
    int ifLine = previous().line;
    int ifCol = previous().column;
    consume(TokenType::LeftParen, "Expected '(' after if");
    auto condition = parseExpression();
    consume(TokenType::RightParen, "Expected ')' after condition");
    consume(TokenType::LeftBrace, "Expected '{' before then branch");

    std::vector<StmtPtr> thenBranch;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            bool isNullExpr = false;
            if (auto* es = dynamic_cast<ExprStmt*>(stmt.get())) {
                if (!es->expr) isNullExpr = true;
            }
            if (isNullExpr) {
                advance();
                continue;
            }
            thenBranch.push_back(std::move(stmt));
        } else {
            advance();
        }
        if (_current == before) advance();  // panic-mode safety net
    }
    consume(TokenType::RightBrace, "Expected '}' after then branch");

    std::vector<StmtPtr> elseBranch;
    if (match(TokenType::Else)) {
        consume(TokenType::LeftBrace, "Expected '{' before else branch");
        while (!check(TokenType::RightBrace) && !isAtEnd()) {
            size_t before = _current;
            if (auto stmt = parseStatement()) {
                bool isNullExpr = false;
                if (auto* es = dynamic_cast<ExprStmt*>(stmt.get())) {
                    if (!es->expr) isNullExpr = true;
                }
                if (isNullExpr) {
                    advance();
                    continue;
                }
                elseBranch.push_back(std::move(stmt));
            } else {
                advance();
            }
            if (_current == before) advance();  // panic-mode safety net
        }
        consume(TokenType::RightBrace, "Expected '}' after else branch");
    }

    auto ifStmt = std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
    ifStmt->line = ifLine;
    ifStmt->column = ifCol;
    return std::move(ifStmt);
}

std::unique_ptr<Stmt> Parser::parseForStmt() {
    consume(TokenType::LeftParen, "Expected '(' after for");
    // IR-H-01: stamp for-stmt location at the loop variable token for
    // iterable-type errors (when e.g. `for (x in 1)` is written with a
    // scalar where a vector is expected — the analyzer reports the
    // problem with the loop-var's location).
    Token variable = consumeName("Expected loop variable");
    int forLine = variable.line;
    int forCol = variable.column;
    consume(TokenType::In, "Expected 'in' after for variable");
    auto iterable = parseExpression();
    consume(TokenType::RightParen, "Expected ')' after iterable");
    consume(TokenType::LeftBrace, "Expected '{' before for body");

    std::vector<StmtPtr> body;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            bool isNullExpr = false;
            if (auto* es = dynamic_cast<ExprStmt*>(stmt.get())) {
                if (!es->expr) isNullExpr = true;
            }
            if (isNullExpr) {
                advance();
                continue;
            }
            body.push_back(std::move(stmt));
        } else {
            advance();
        }
        if (_current == before) advance();  // panic-mode safety net
    }
    consume(TokenType::RightBrace, "Expected '}' after for body");

    auto forStmt = std::make_unique<ForStmt>(variable.lexeme, std::move(iterable), std::move(body));
    forStmt->line = forLine;
    forStmt->column = forCol;
    return std::move(forStmt);
}

const Token& Parser::current() const {
    return _tokens[_current];
}

const Token& Parser::previous() const {
    return _tokens[_current - 1];
}

bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return current().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::advance() {
    if (!isAtEnd()) _current++;
    return previous();
}

bool Parser::isAtEnd() const {
    return current().type == TokenType::EndOfFile;
}

void Parser::error(const std::string& message) {
    const Token& tok = current();
    _reporter.error(ErrorCode::UnexpectedToken, message, tok.line, tok.column);
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    error(message);
    return previous();
}

Token Parser::consumeTypeName(const std::string& message) {
    // Phase 2 Step 5: builtin type names are now plain Identifier tokens
    // (see AYShader\Token.h / design.md §11.1). We just consume an Identifier
    // and let the caller validate via AYBuiltinTypes::isBuiltinType.
    // The retained name `consumeTypeName` (rather than inlining a
    // consume(Identifier, ...)) keeps the call sites self-documenting.
    if (check(TokenType::Identifier)) return advance();
    error(message);
    // Force-advance so the loop doesn't spin (see also the safety net
    // in parse()).
    return advance();
}

// ----- Audit 2026-08-26 P-M-01..03: robust integer literal parsing -----
//
// The pre-fix parser used `std::stol(...)` directly on the literal
// lexeme. Two robustness problems resulted:
//
//   1. (P-M-02) std::stol defaults to base-10, so a hex/octal/binary
//      literal like `0xFF` or `0b1010` would throw std::invalid_argument
//      and the downstream parser would emit a confusing
//      "must be a non-negative integer" error. Lexer 2026-08-26 keeps
//      these prefixes verbatim in the IntLiteral lexeme.
//
//   2. (P-M-01) std::stol silently accepts negative values and numbers
//      larger than 32 bits, leaving the parser to do defensive checks
//      scattered across parseUniformDecl / parseSharedDecl /
//      parseStorageDecl / parseUniformBlockDecl. Some call sites
//      didn't even check — for example, parseStorageDecl's binding
//      silently truncated a user-written `99999999999` to int.
//
// The fix is one helper, parseIntLiteral(), that:
//   - handles signed leading +/-
//   - dispatches to the right base via the 0x / 0b / 0o prefix
//   - clamps on overflow but flags it as out-of-range via the return
//     tuple (parsedOutOfRange=true)
//   - returns a (value, ok, parsedOutOfRange) triple so call sites
//     produce their own context-appropriate error messages via the
//     existing Parser::error() path
namespace {

struct IntLiteralResult {
    long long value;
    bool ok;                  // false → lexeme wasn't a valid int
    bool parsedOutOfRange;    // true → overflow or clamp
};

static IntLiteralResult parseIntLiteral(const std::string& lexeme) {
    IntLiteralResult r{0, false, false};
    if (lexeme.empty()) return r;
    const char* p = lexeme.c_str();
    bool hadSign = false;
    if (*p == '+') {
        ++p; hadSign = true;
    } else if (*p == '-') {
        ++p; hadSign = true;
    }
    int base = 10;
    // Prefix detection — only when the leading char is '0' AND there's
    // another char AND that char isn't a digit (so plain `0` / `01`
    // pass through). Phoskia today does not let the user write negative
    // int literals at statement position (the lexer doesn't synthesize
    // `-1` into a single IntLiteral); the sign handling above only
    // guards against future change and unusual sources.
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (*p == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;
        p += 2;
    } else if (*p == '0' && (p[1] == 'o' || p[1] == 'O')) {
        base = 8;
        p += 2;
    } else if (*p == '0' && (p[1] == '\0')) {
        // Literal "0" → value 0, no error.
        r.value = 0;
        r.ok = true;
        return r;
    }
    // Empty after prefix → invalid (e.g. "0x" with no digits). Fall
    // through to strtoll which will set end==p, indicating no digits.
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(p, &end, base);
    if (end == p) {
        // No digits consumed. Lexer shouldn't have produced this
        // for an IntLiteral, but we guard anyway.
        return r;
    }
    // Reject trailing junk like "12abc" — strtoll silently stops at
    // the first non-digit. We strictly require end==string-end.
    if (*end != '\0') return r;
    if (errno == ERANGE) {
        r.value = (v < 0) ? std::numeric_limits<long long>::min()
                          : std::numeric_limits<long long>::max();
        r.parsedOutOfRange = true;
        r.ok = true;
        return r;
    }
    r.value = v;
    r.ok = true;
    return r;
}

}  // namespace

// Wrapper exposed for the call sites to use. Validates that the
// parsed value is non-negative and within `[minInclusive,
// maxInclusive]`. On failure: returns the failure kind so the
// caller can decide whether to error() and bail or to error() and
// keep parsing.
enum class IntParseFailure {
    None,
    InvalidLexeme,
    Overflow,
    OutOfRange,
    Negative,
};

struct IntParseResult {
    int value;
    IntParseFailure failure;
};

namespace {

// (kept anon for `parseIntLiteral`.)

}  // namespace

IntParseResult parseSignedIntInRange(
        const std::string& lexeme,
        int minInclusive,
        int maxInclusive,
        bool requireNonNegative) {
    IntParseResult out{0, IntParseFailure::None};
    IntLiteralResult pi = parseIntLiteral(lexeme);
    if (!pi.ok) {
        out.failure = IntParseFailure::InvalidLexeme;
        return out;
    }
    if (pi.parsedOutOfRange) {
        out.failure = IntParseFailure::Overflow;
        return out;
    }
    long long v = pi.value;
    if (requireNonNegative && v < 0) {
        out.failure = IntParseFailure::Negative;
        return out;
    }
    if (v < static_cast<long long>(minInclusive) ||
        v > static_cast<long long>(maxInclusive)) {
        out.failure = IntParseFailure::OutOfRange;
        return out;
    }
    out.value = static_cast<int>(v);
    return out;
}

static int parseNonNegativeInt(
        const std::string& lexeme,
        int maxInclusive,
        std::function<void(const std::string&)> reporter,
        const std::string& fieldLabel,
        bool& failed) {
    IntParseResult r = parseSignedIntInRange(lexeme, 0, maxInclusive, true);
    if (r.failure != IntParseFailure::None) {
        switch (r.failure) {
            case IntParseFailure::InvalidLexeme:
                reporter(fieldLabel + " must be a non-negative integer, got '"
                         + lexeme + "'");
                break;
            case IntParseFailure::Overflow:
                reporter(fieldLabel + " value is out of representable range (got '"
                         + lexeme + "')");
                break;
            case IntParseFailure::Negative:
                reporter(fieldLabel + " must be non-negative (got '"
                         + lexeme + "')");
                break;
            case IntParseFailure::OutOfRange:
                reporter(fieldLabel + " out of [0.." + std::to_string(maxInclusive)
                         + "] range (got '" + lexeme + "')");
                break;
            default: break;
        }
        failed = true;
        return 0;
    }
    failed = false;
    return r.value;
}

static int parsePositiveInt(
        const std::string& lexeme,
        int maxInclusive,
        std::function<void(const std::string&)> reporter,
        const std::string& fieldLabel,
        bool& failed) {
    IntParseResult r = parseSignedIntInRange(lexeme, 1, maxInclusive, true);
    if (r.failure != IntParseFailure::None) {
        switch (r.failure) {
            case IntParseFailure::InvalidLexeme:
                reporter(fieldLabel + " must be a positive integer, got '"
                         + lexeme + "'");
                break;
            case IntParseFailure::Overflow:
                reporter(fieldLabel + " value is out of representable range (got '"
                         + lexeme + "')");
                break;
            case IntParseFailure::Negative:
                // Treat negative as "must be positive" too.
                reporter(fieldLabel + " must be positive (got '"
                         + lexeme + "')");
                break;
            case IntParseFailure::OutOfRange:
                reporter(fieldLabel + " out of [1.." + std::to_string(maxInclusive)
                         + "] range (got '" + lexeme + "')");
                break;
            default: break;
        }
        failed = true;
        return 0;
    }
    failed = false;
    return r.value;
}

Token Parser::consumeName(const std::string& message) {
    // Phase 1: many reserved keywords (Phoskia semantic types
    // position/normal/color/texcoord, plus In/Out/etc.) can appear in
    // "name position" — accept any of them as a valid name token.
    if (check(TokenType::Identifier) ||
        check(TokenType::Position) || check(TokenType::Normal) ||
        check(TokenType::Color) || check(TokenType::Texcoord) ||
        check(TokenType::In) || check(TokenType::Out)) {
        return advance();
    }
    error(message);
    return previous();
}

int Parser::getPrecedence(TokenType op) {
    switch (op) {
        // '=' is right-associative and lowest precedence — `a = b = c` parses
        // as `a = (b = c)` semantically. We assign it precedence 1 (the same
        // as `||`) but mark it via a separate RightAssoc check below so the
        // parser loop treats it correctly.
        case TokenType::Equal: return 1;
        case TokenType::Or: return 1;
        case TokenType::And: return 2;
        case TokenType::EqualEqual:
        case TokenType::BangEqual: return 3;
        case TokenType::Less:
        case TokenType::LessEqual:
        case TokenType::Greater:
        case TokenType::GreaterEqual: return 4;
        case TokenType::Plus:
        case TokenType::Minus: return 5;
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent: return 6;
        default: return 0;
    }
}

// ----- Phase 2 Step 4: panic-mode recovery -----

void Parser::synchronize() {
    // Advance until we land on a top-level statement boundary. A boundary
    // token is one that can start a fresh declaration / statement:
    //   - material / property / uniform / texture2d / vertex / fragment
    //   - let / return / if / for
    //   - left-brace (start of a stray block)
    //   - left-bracket (start of [variant ...])
    //   - right-brace (closing a block we couldn't open)
    //   - EOF
    //
    // We don't re-report an error here — the original error was already
    // surfaced by the failed parseStatement() / consume() call that
    // triggered this recovery. Re-reporting would spam the error stream.
    while (!isAtEnd()) {
        switch (current().type) {
            case TokenType::Material:
            case TokenType::Compute:
            case TokenType::Property:
            case TokenType::Uniform:
            case TokenType::Texture2D:
            case TokenType::TextureCube:
            case TokenType::Vertex:
            case TokenType::Fragment:
            case TokenType::Let:
            case TokenType::Return:
            case TokenType::If:
            case TokenType::For:
            case TokenType::LeftBrace:
            case TokenType::LeftBracket:
            case TokenType::RightBrace:
                return;
            default:
                advance();
        }
    }
}

void Parser::synchronizeToBlockEnd() {
    // Advance until we land on a right-brace (the close of the current
    // vertex / fragment / if / for body) or EOF. Used after an error
    // inside a block body so the parser can finish parsing the body
    // and the surrounding declaration cleanly.
    while (!isAtEnd() && current().type != TokenType::RightBrace) {
        advance();
    }
}

} // namespace ayt::shader::phoskia
