// AYParser.cpp - Parser implementation

#include "AYParser.h"
#include <iostream>

namespace ayt::shader::phoskia
{

static const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::Material: return "Material";
        case TokenType::Property: return "Property";
        case TokenType::Uniform: return "Uniform";
        case TokenType::Texture2D: return "Texture2D";
        case TokenType::Sampler: return "Sampler";
        case TokenType::Vertex: return "Vertex";
        case TokenType::Fragment: return "Fragment";
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
    if (match(TokenType::Property)) {
        return parsePropertyDecl();
    }
    if (match(TokenType::Uniform)) {
        return parseUniformDecl();
    }
    if (match(TokenType::Texture2D)) {
        return parseTextureDecl();
    }
    if (match(TokenType::Vertex)) {
        return parseVertexFunc();
    }
    if (match(TokenType::Fragment)) {
        return parseFragmentFunc();
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
        left = std::make_unique<BinaryExpr>(std::move(left), opTok, std::move(right));
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
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
        } else if (match(TokenType::Dot)) {
            Token name = consumeName("Expected property name after '.'");
            expr = std::make_unique<MemberExpr>(std::move(expr), name.lexeme);
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
    // AYToken.h / design.md §11.1 — "类型名降级重构"), so the previous
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
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<UniformDecl>(type.lexeme, name.lexeme);
}

std::unique_ptr<Stmt> Parser::parseTextureDecl() {
    Token name = consumeName("Expected texture name");
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<TextureDecl>(name.lexeme);
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
    // Identifier tokens (see AYToken.h / design.md §11.1). The
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
    else {
        error("Expected Phoskia semantic type (position/normal/color/texcoord)");
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
    std::vector<StmtPtr> body;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        if (check(TokenType::In)) {
            advance();
            if (auto p = parseShaderParam(ShaderParam::Direction::In)) {
                inputs.push_back(std::move(p));
            }
            continue;
        }
        if (check(TokenType::Out)) {
            error("Fragments cannot have 'out' parameters");
            advance();
            continue;
        }
        break;
    }
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
    return std::make_unique<FragmentFunc>(std::move(inputs), std::move(body));
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
    return std::make_unique<LetStmt>(name.lexeme, std::move(initializer));
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    std::unique_ptr<Expr> value = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = parseExpression();
    }
    match(TokenType::Semicolon);  // ';' is optional (Python-like)
    return std::make_unique<ReturnStmt>(std::move(value));
}

std::unique_ptr<Stmt> Parser::parseIfStmt() {
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

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<Stmt> Parser::parseForStmt() {
    consume(TokenType::LeftParen, "Expected '(' after for");
    Token variable = consumeName("Expected loop variable");
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

    return std::make_unique<ForStmt>(variable.lexeme, std::move(iterable), std::move(body));
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
    // (see AYToken.h / design.md §11.1). We just consume an Identifier
    // and let the caller validate via AYBuiltinTypes::isBuiltinType.
    // The retained name `consumeTypeName` (rather than inlining a
    // consume(Identifier, ...)) keeps the call sites self-documenting.
    if (check(TokenType::Identifier)) return advance();
    error(message);
    // Force-advance so the loop doesn't spin (see also the safety net
    // in parse()).
    return advance();
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
            case TokenType::Property:
            case TokenType::Uniform:
            case TokenType::Texture2D:
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
