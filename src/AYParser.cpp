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
        case TokenType::Shading: return "Shading";
        case TokenType::Vertex: return "Vertex";
        case TokenType::Fragment: return "Fragment";
        case TokenType::Let: return "Let";
        case TokenType::If: return "If";
        case TokenType::Else: return "Else";
        case TokenType::For: return "For";
        case TokenType::In: return "In";
        case TokenType::Return: return "Return";
        case TokenType::True: return "True";
        case TokenType::False: return "False";
        case TokenType::Float: return "Float";
        case TokenType::Vec2: return "Vec2";
        case TokenType::Vec3: return "Vec3";
        case TokenType::Vec4: return "Vec4";
        case TokenType::Int: return "Int";
        case TokenType::IVec2: return "IVec2";
        case TokenType::IVec3: return "IVec3";
        case TokenType::IVec4: return "IVec4";
        case TokenType::Mat2: return "Mat2";
        case TokenType::Mat3: return "Mat3";
        case TokenType::Mat4: return "Mat4";
        case TokenType::Quat: return "Quat";
        case TokenType::Bool: return "Bool";
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
        std::cerr << "[parse] before=" << before
                  << " curType=" << tokenTypeName(current().type)
                  << " lexeme='" << current().lexeme << "'\n";
        if (auto stmt = parseStatement()) {
            program->declarations.push_back(std::move(stmt));
        } else {
            // Error recovery: skip to next statement boundary
            advance();
        }
        std::cerr << "[parse] after=" << _current
                  << " curType=" << tokenTypeName(current().type)
                  << " lexeme='" << current().lexeme << "'\n";
        if (_current == before) {
            std::cerr << "[parse] STUCK — force advancing\n";
            advance();
        }
    }
    return program;
}

std::unique_ptr<Stmt> Parser::parseStatement() {
    std::cerr << "[parseStatement] entry _current=" << _current
              << " curType=" << tokenTypeName(current().type)
              << " lexeme='" << current().lexeme << "'\n";
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
    if (match(TokenType::Shading)) {
        return parseShadingFunc();
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
        if (nextPrecedence <= precedence) break;

        advance();
        auto right = parseBinary(nextPrecedence);
        left = std::make_unique<BinaryExpr>(std::move(left), previous(), std::move(right));
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
    std::cerr << "[parseCall] entry _current=" << _current
              << " curType=" << tokenTypeName(current().type) << "\n";
    auto expr = parsePrimary();

    while (true) {
        if (match(TokenType::LeftParen)) {
            std::cerr << "[parseCall] saw ( _current=" << _current << "\n";
            std::vector<ExprPtr> args;
            if (!check(TokenType::RightParen)) {
                do {
                    args.push_back(parseExpression());
                } while (match(TokenType::Comma));
            }
            consume(TokenType::RightParen, "Expected ')' after arguments");
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
        } else if (match(TokenType::Dot)) {
            Token name = consume(TokenType::Identifier, "Expected property name after '.'");
            expr = std::make_unique<MemberExpr>(std::move(expr), name.lexeme);
        } else if (match(TokenType::LeftBracket)) {
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
    std::cerr << "[parsePrimary] entry _current=" << _current
              << " curType=" << tokenTypeName(current().type)
              << " lexeme='" << current().lexeme << "'\n";
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
    // ===== Phase 1 workaround (see design.md §11.1) =====
    // Builtin type names like `vec3`, `mat4`, `bool` are emitted by the
    // Lexer as dedicated keywords (TokenType::Vec3, Mat4, Bool, ...). In
    // expression context they appear as constructor calls — `vec3(1,0,0)`,
    // `mat4()`, `bool(...)`. Without these branches parsePrimary fails and
    // the parser spins on an ExprStmt(nullptr). Phase 2 will demote the
    // type tokens to Identifier (Go/Swift style); these branches are then
    // removed.
    if (match(TokenType::Float) || match(TokenType::Vec2) || match(TokenType::Vec3) ||
        match(TokenType::Vec4) || match(TokenType::Int) || match(TokenType::IVec2) ||
        match(TokenType::IVec3) || match(TokenType::IVec4) || match(TokenType::Mat2) ||
        match(TokenType::Mat3) || match(TokenType::Mat4) || match(TokenType::Quat) ||
        match(TokenType::Bool)) {
        return std::make_unique<IdentifierExpr>(previous().lexeme);
    }
    // ===== End Phase 1 workaround =====
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
    std::cerr << "[parseMaterialDecl] entry _current=" << _current << "\n";
    Token name = consume(TokenType::Identifier, "Expected material name");
    consume(TokenType::LeftBrace, "Expected '{' before material body");

    std::vector<StmtPtr> declarations;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        std::cerr << "[parseMaterialDecl.loop] _current=" << _current
                  << " curType=" << tokenTypeName(current().type) << "\n";
        size_t before = _current;
        if (auto stmt = parseStatement()) {
            declarations.push_back(std::move(stmt));
        } else {
            advance();
        }
        if (_current == before) {
            std::cerr << "[parseMaterialDecl.loop] STUCK — force advancing\n";
            advance();  // panic-mode safety net
        }
    }

    consume(TokenType::RightBrace, "Expected '}' after material body");
    return std::make_unique<MaterialDecl>(name.lexeme, std::move(declarations));
}

std::unique_ptr<Stmt> Parser::parsePropertyDecl() {
    std::cerr << "[parsePropertyDecl] entry _current=" << _current << "\n";
    Token name = consume(TokenType::Identifier, "Expected property name");
    std::cerr << "[parsePropertyDecl] after name _current=" << _current << "\n";
    consume(TokenType::Equal, "Expected '=' after property name");
    std::cerr << "[parsePropertyDecl] before expr _current=" << _current
              << " curType=" << tokenTypeName(current().type) << "\n";
    auto initializer = parseExpression();
    std::cerr << "[parsePropertyDecl] after expr _current=" << _current
              << " curType=" << tokenTypeName(current().type) << "\n";
    consume(TokenType::Semicolon, "Expected ';' after property");
    std::cerr << "[parsePropertyDecl] after ; _current=" << _current << "\n";
    return std::make_unique<PropertyDecl>(name.lexeme, std::move(initializer));
}

std::unique_ptr<Stmt> Parser::parseUniformDecl() {
    // Phase 1: also accept builtin type keywords (Float/Vec2/.../Bool) since
    // Lexer emits them as dedicated tokens. Phase 2's "demote to Identifier"
    // refactor (design.md §11.1) will let us revert to a single Identifier
    // consume() once the Lexer change lands.
    Token type = consumeTypeName("Expected uniform type");
    Token name = consume(TokenType::Identifier, "Expected uniform name");
    consume(TokenType::Semicolon, "Expected ';' after uniform");
    return std::make_unique<UniformDecl>(type.lexeme, name.lexeme);
}

std::unique_ptr<Stmt> Parser::parseTextureDecl() {
    Token name = consume(TokenType::Identifier, "Expected texture name");
    consume(TokenType::Semicolon, "Expected ';' after texture");
    return std::make_unique<TextureDecl>(name.lexeme);
}

std::unique_ptr<Stmt> Parser::parseShadingFunc() {
    consume(TokenType::LeftBrace, "Expected '{' before shading body");
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

    consume(TokenType::RightBrace, "Expected '}' after shading body");
    return std::make_unique<ShadingFunc>(std::move(body));
}

std::unique_ptr<Stmt> Parser::parseVariantAttribute() {
    // We've already consumed '[' via match(). Expect: identifier (variant name) ']'
    if (!check(TokenType::Identifier)) {
        error("Expected variant name after '['");
        return nullptr;
    }
    Token name = current();
    advance();
    consume(TokenType::RightBracket, "Expected ']' after variant name");
    return std::make_unique<VariantAttribute>(name.lexeme);
}

std::unique_ptr<Stmt> Parser::parseLetStmt() {
    Token name = consume(TokenType::Identifier, "Expected variable name");
    consume(TokenType::Equal, "Expected '=' after let");
    auto initializer = parseExpression();
    consume(TokenType::Semicolon, "Expected ';' after let");
    return std::make_unique<LetStmt>(name.lexeme, std::move(initializer));
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    std::unique_ptr<Expr> value = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = parseExpression();
    }
    consume(TokenType::Semicolon, "Expected ';' after return");
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
    Token variable = consume(TokenType::Identifier, "Expected loop variable");
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
    // Accept Identifier OR any builtin type keyword. If neither, report the
    // error and force-advance so the parser does not spin on the same token.
    if (check(TokenType::Identifier) ||
        check(TokenType::Float) || check(TokenType::Vec2) || check(TokenType::Vec3) ||
        check(TokenType::Vec4) || check(TokenType::Int) || check(TokenType::IVec2) ||
        check(TokenType::IVec3) || check(TokenType::IVec4) || check(TokenType::Mat2) ||
        check(TokenType::Mat3) || check(TokenType::Mat4) || check(TokenType::Quat) ||
        check(TokenType::Bool)) {
        return advance();
    }
    error(message);
    // Force-advance so the loop doesn't spin (see also the safety net in
    // parse()).
    return advance();
}

int Parser::getPrecedence(TokenType op) {
    switch (op) {
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

} // namespace ayt::shader::phoskia
