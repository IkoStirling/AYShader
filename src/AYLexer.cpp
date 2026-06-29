// AYLexer.cpp - Lexer implementation

#include "AYLexer.h"
#include <unordered_map>
#include <iostream>

namespace ayt::shader::phoskia
{

Lexer::Lexer(const std::string& source)
    : _source(source), _start(0), _current(0), _line(1), _column(1) {}

void Lexer::tokenize(std::vector<Token>& out) {
    // Reserve enough capacity up front so the underlying buffer never
    // reallocates while we push tokens. The Token type is small (TokenType
    // + line + column + a std::string lexeme) — once the vector's buffer is
    // reserved, push_back is a placement-new on already-allocated memory
    // and never moves already-pushed tokens.
    //
    // We append into `out` (caller-owned) rather than building into a
    // member vector and returning it. Returning std::vector<Token> by value
    // would force a vector move at the call site, and on MSVC that move
    // can corrupt Token::type fields when an SSO std::string is moved
    // (the move's proxy-handle handling overwrites adjacent memory). Using
    // an out-parameter means the caller's vector grows in place — no move
    // of any Token element happens after construction.
    out.reserve(out.size() + 1024);

    while (!isAtEnd()) {
        _start = _current;
        scanToken(out);
    }
    Token eof;
    eof.type = TokenType::EndOfFile;
    eof.lexeme = "";
    eof.line = _line;
    eof.column = _column;
    out.push_back(eof);
}

void Lexer::scanToken(std::vector<Token>& out) {
    char c = advance();
    switch (c) {
        case '(': makeToken(out, TokenType::LeftParen, 1); break;
        case ')': makeToken(out, TokenType::RightParen, 1); break;
        case '{': makeToken(out, TokenType::LeftBrace, 1); break;
        case '}': makeToken(out, TokenType::RightBrace, 1); break;
        case '[': makeToken(out, TokenType::LeftBracket, 1); break;
        case ']': makeToken(out, TokenType::RightBracket, 1); break;
        case ',': makeToken(out, TokenType::Comma, 1); break;
        case ':': makeToken(out, TokenType::Colon, 1); break;
        case ';': makeToken(out, TokenType::Semicolon, 1); break;
        case '.': makeToken(out, TokenType::Dot, 1); break;
        case '+': makeToken(out, TokenType::Plus, 1); break;
        case '-': makeToken(out, TokenType::Minus, 1); break;
        case '*': makeToken(out, TokenType::Star, 1); break;
        case '/': makeToken(out, TokenType::Slash, 1); break;
        case '%': makeToken(out, TokenType::Percent, 1); break;
        case '=': makeToken(out, match('=') ? TokenType::EqualEqual : TokenType::Equal, 1); break;
        case '!': makeToken(out, match('=') ? TokenType::BangEqual : TokenType::Bang, 1); break;
        case '<': makeToken(out, match('=') ? TokenType::LessEqual : TokenType::Less, 1); break;
        case '>': makeToken(out, match('=') ? TokenType::GreaterEqual : TokenType::Greater, 1); break;
        case '&': makeToken(out, match('&') ? TokenType::And : TokenType::Unknown, 1); break;
        case '|': makeToken(out, match('|') ? TokenType::Or : TokenType::Unknown, 1); break;
        case '#': {
            // Phoskia doesn't use `#` in its surface syntax — swallow it
            // silently so stray `#`s (e.g. in commented-out source) don't
            // pollute the token stream as Unknown tokens. A genuine
            // syntax error surfaces in the parser when the surrounding
            // tokens don't make sense.
            break;
        }
        case '"': stringLiteral(out); break;
        case ' ':
        case '\r':
        case '\t':
            break;
        case '\n':
            std::cerr << "[scanToken] saw \\n before= _line=" << _line
                      << " _column=" << _column << " _current=" << _current << "\n";
            _line++;
            _column = 0;
            std::cerr << "[scanToken] saw \\n after= _line=" << _line
                      << " _column=" << _column << "\n";
            break;
        default:
            if (isdigit(c)) {
                number(out);
            } else if (isalpha(c) || c == '_') {
                identifier(out);
            } else {
                makeToken(out, TokenType::Unknown, 1);
            }
    }
}

Token Lexer::makeToken(std::vector<Token>& out, TokenType type, int length) {
    Token token;
    token.type = type;
    token.lexeme = _source.substr(_start, length);
    token.line = _line;
    // Column is 1-based: the first character on a line is column 1.
    // `_column` has already been advanced past the last character of this
    // token, so the token's starting column is `_column - length + 1`.
    token.column = _column - length + 1;
    out.push_back(token);
    return token;
}

TokenType Lexer::identifierType(const std::string& lexeme) {
    static const std::unordered_map<std::string, TokenType> keywords = {
        {"material", TokenType::Material},
        {"property", TokenType::Property},
        {"uniform", TokenType::Uniform},
        {"texture2d", TokenType::Texture2D},
        {"sampler", TokenType::Sampler},
        {"vertex", TokenType::Vertex},
        {"fragment", TokenType::Fragment},
        {"compute", TokenType::Compute},
        {"let", TokenType::Let},
        {"if", TokenType::If},
        {"else", TokenType::Else},
        {"for", TokenType::For},
        {"in", TokenType::In},
        {"out", TokenType::Out},
        {"return", TokenType::Return},
        {"true", TokenType::True},
        {"false", TokenType::False},
        {"variant", TokenType::Variant},
        // Phoskia semantic types
        {"position", TokenType::Position},
        {"normal", TokenType::Normal},
        {"color", TokenType::Color},
        {"texcoord", TokenType::Texcoord},
        // GLSL type names are intentionally NOT keywords — they are
        // emitted as plain Identifier tokens (lexeme = "vec3", "float",
        // ...). Whether a given Identifier is a builtin type is decided
        // by the parser / semantic analyzer via AYBuiltinTypes::isBuiltinType.
        // See design.md §11.1 — "类型名降级重构".
    };

    auto it = keywords.find(lexeme);
    return (it != keywords.end()) ? it->second : TokenType::Identifier;
}

Token Lexer::number(std::vector<Token>& out) {
    while (isdigit(peek())) advance();

    // Decide whether this is an integer or float literal based on whether
    // a fractional part follows. "42" → IntLiteral, "3.14" → FloatLiteral.
    // A trailing "." without following digits (e.g. "0.") is treated as
    // an integer followed by a Dot operator, matching the
    // `number_zero_point_not_float` test expectation.
    bool hasFraction = false;
    if (peek() == '.' && isdigit(peekNext())) {
        hasFraction = true;
        advance();
        while (isdigit(peek())) advance();
    }

    Token token;
    token.type = hasFraction ? TokenType::FloatLiteral : TokenType::IntLiteral;
    token.lexeme = _source.substr(_start, _current - _start);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) + 1;
    // Note: numeric conversion is deferred to the Parser. The Lexer only
    // captures the lexeme; turning "3.14" into a float is a semantic concern.
    out.push_back(token);
    return token;
}

Token Lexer::identifier(std::vector<Token>& out) {
    while (isalnum(peek()) || peek() == '_') advance();

    Token token;
    token.lexeme = _source.substr(_start, _current - _start);
    token.type = identifierType(token.lexeme);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) + 1;
    out.push_back(token);
    return token;
}

Token Lexer::stringLiteral(std::vector<Token>& out) {
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') {
            _line++;
            _column = 0;
        }
        advance();
    }

    if (isAtEnd()) {
        return makeToken(out, TokenType::Unknown, _current - _start);
    }

    advance();  // closing "
    Token token;
    token.type = TokenType::StringLiteral;
    // Lexeme carries the *raw* substring between the quotes (no escape
    // processing). Escape handling is the Parser's job.
    token.lexeme = _source.substr(_start + 1, _current - _start - 2);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) - 2 + 1;
    out.push_back(token);
    return token;
}

char Lexer::advance() {
    _current++;
    _column++;
    return _source[_current - 1];
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (_source[_current] != expected) return false;
    _current++;
    _column++;
    return true;
}

bool Lexer::isAtEnd() const {
    return _current >= static_cast<int>(_source.length());
}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return _source[_current];
}

char Lexer::peekNext() const {
    if (_current + 1 >= static_cast<int>(_source.length())) return '\0';
    return _source[_current + 1];
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\r' || c == '\t') {
            advance();
        } else if (c == '\n') {
            _line++;
            _column = 0;
            advance();
        } else {
            break;
        }
    }
}

} // namespace ayt::shader::phoskia
