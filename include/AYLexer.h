#pragma once
// AYLexer.h - Lexer for Phoskia shader language

#include "AYToken.h"
#include <vector>
#include <string>

namespace ayt::shader::phoskia
{

class Lexer {
public:
    explicit Lexer(const std::string& source);

    // Tokenize the source and append tokens to `out`. Using an out-parameter
    // instead of returning std::vector<Token> by value avoids a vector move
    // on the call site, which on MSVC can trigger a SSO string-move bug
    // that corrupts Token::type fields (see design.md §6.5 / Phase 2
    // notes). The caller owns `out` and is responsible for its lifetime;
    // we only push into it.
    void tokenize(std::vector<Token>& out);

private:
    void scanToken(std::vector<Token>& out);
    Token makeToken(std::vector<Token>& out, TokenType type, int length);
    TokenType identifierType(const std::string& lexeme);
    void skipWhitespace();
    char advance();
    bool match(char expected);
    bool isAtEnd() const;
    char peek() const;
    char peekNext() const;

    Token number(std::vector<Token>& out);
    Token identifier(std::vector<Token>& out);
    Token stringLiteral(std::vector<Token>& out);

    std::string _source;
    int _start = 0;
    int _current = 0;
    int _line = 1;
    int _column = 1;
};

} // namespace ayt::shader::phoskia
