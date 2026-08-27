// AYLexer.cpp - Lexer implementation

#include "AYShader/Lexer.h"
#include <cctype>
#include <unordered_map>
#include <iostream>

namespace ayt::shader::phoskia
{

namespace {

// MSVC Debug CRT asserts when isdigit/isalpha see a negative signed char
// (UTF-8 continuation bytes from comments / pasted source). Always widen
// through unsigned char first.
inline bool isDigitByte(char c)  { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
inline bool isAlphaByte(char c)  { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
inline bool isAlnumByte(char c)  { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

} // namespace

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
    //
    // L-H-01: reset the cursor state at the start of every call. The Lexer
    // is documented as a reusable object — production callers usually
    // construct a fresh Lexer per source, but tests (and any pool that
    // reuses the same Lexer across compiles) rely on state being cleared.
    // Without this reset, the second `tokenize` call on the same instance
    // would resume mid-source, skip the prefix, and emit EOF at the wrong
    // position.
    _start = 0;
    _current = 0;
    _line = 1;
    _column = 1;

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
        case '/': {
            // Line comments: `// ...` through end of line. Without this,
            // UTF-8 bytes in comments hit isdigit() as signed char and
            // trip the Debug CRT assert (ucrtbased.dll).
            if (match('/')) {
                while (peek() != '\n' && !isAtEnd()) {
                    advance();
                }
            }
            // L-B-01: block comments `/* ... */`. Without this, any
            // shader source containing a block comment (the bgfx toolchain
            // emits plenty in its examples, and our own golden files use
            // them in shader headers) hits a hard Unknown-token error in
            // the parser. Block comments may span multiple lines and may
            // also nest in GLSL/HLSL — Phoskia follows GLSL's nesting rule.
            else if (match('*')) {
                int nesting = 1;
                while (nesting > 0 && !isAtEnd()) {
                    if (peek() == '/' && peekNext() == '*') {
                        // Nested `/*` — bump depth.
                        advance();
                        advance();
                        ++nesting;
                    } else if (peek() == '*' && peekNext() == '/') {
                        // Closing `*/` — drop depth (even at 0 we let the
                        // outer `nesting > 0` check terminate the loop).
                        advance();
                        advance();
                        --nesting;
                    } else {
                        if (peek() == '\n') {
                            _line++;
                            _column = 0;
                        }
                        advance();
                    }
                }
                // Unterminated block comment: surface as a TokenType::Unknown
                // spanning from the leading `/` through the EOF position so
                // diagnostic line/column in the parser makes sense.
                if (nesting > 0) {
                    makeToken(out, TokenType::Unknown, _current - _start);
                }
            } else {
                makeToken(out, TokenType::Slash, 1);
            }
            break;
        }
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
            _line++;
            _column = 0;
            break;
        default:
            if (isDigitByte(c)) {
                number(out);
            } else if (isAlphaByte(c) || c == '_') {
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
        {"storage", TokenType::Storage},
        // Phase 3.3 Block 4: workgroup-shared local memory.
        //   shared <type> <name>[<size>];
        // Lowers to GLSL `shared <type> <name>[<size>];` inside a
        // compute body. The keyword name matches GLSL exactly (HLSL
        // uses `groupshared`; the bgfx GLSL profile accepts `shared`).
        {"shared", TokenType::Shared},
        // Phase 3.4: top-level uniform buffer object.
        //   uniformblock <Name> { <type> <field>; ... }
        // Lowers to GLSL `layout(std140, binding = N) uniform <Name>
        // { <type> <field>; ... } <Name>;`. The block name doubles as
        // the instance name (GLSL convention) — users access fields
        // as `<Name>.<field>` from shader bodies.
        {"uniformblock", TokenType::UniformBlock},
        // Phase 3.5-A: storage decl explicit binding slot.
        //   storage NAME : rwstructuredbuffer<T> binding N;
        // The keyword is `binding` (not `slot` / `index`) to keep
        // 1:1 alignment with GLSL `layout(std430, binding = N)`.
        // The keyword makes it unambiguous vs user identifiers (a
        // user can still have a uniform / variable named `binding`
        // — the keyword form is only recognized at the start of an
        // explicit binding suffix, where a regular identifier would
        // be a syntax error anyway).
        {"binding", TokenType::Binding},
        {"texture2d", TokenType::Texture2D},
        {"texturecube", TokenType::TextureCube},
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
        // Phase 1 RD-03: skeletal skinning vertex semantics.
        // Map to bgfx::Attrib::Indices (4x u8 integer, not normalized) and
        // bgfx::Attrib::Weight (4x f32) in the BGFX converter.
        {"boneindices", TokenType::BoneIndices},
        {"boneweights", TokenType::BoneWeights},
        {"tangent", TokenType::Tangent},
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
    // L-M-02: hex integer literals `0xDEADBEEF` / `0x1F`. GLSL accepts hex
    // constants and Phoskia inherits that. Recognize the prefix before the
    // decimal-loop runs — otherwise the leading `0` would terminate
    // immediately and the rest of the digits would parse as identifiers.
    bool isHex = false;
    if (_current - _start == 1 && peek() == 'x' && _source[_start] == '0') {
        isHex = true;
        advance();  // consume 'x'
        while (true) {
            char c = peek();
            if (isDigitByte(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                advance();
            } else {
                break;
            }
        }
    } else {
        while (isDigitByte(peek())) advance();

        // Decide whether this is an integer or float literal based on whether
        // a fractional part follows. "42" → IntLiteral, "3.14" → FloatLiteral.
        // A trailing "." without following digits (e.g. "0.") is treated as
        // an integer followed by a Dot operator, matching the
        // `number_zero_point_not_float` test expectation.
        bool hasFraction = false;
        if (peek() == '.' && isDigitByte(peekNext())) {
            hasFraction = true;
            advance();
            while (isDigitByte(peek())) advance();
        }

        // L-M-01: scientific notation `1.5e10` / `2.0E-3` / `1e+5`. GLSL
        // requires an exponent on floats; Phoskia follows the same rule —
        // any exponent on an *integer* literal is treated as a FloatLiteral.
        if (peek() == 'e' || peek() == 'E') {
            hasFraction = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (isDigitByte(peek())) advance();
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

    Token token;
    token.type = TokenType::IntLiteral;
    token.lexeme = _source.substr(_start, _current - _start);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) + 1;
    out.push_back(token);
    return token;
}

Token Lexer::identifier(std::vector<Token>& out) {
    while (isAlnumByte(peek()) || peek() == '_') advance();

    Token token;
    token.lexeme = _source.substr(_start, _current - _start);
    token.type = identifierType(token.lexeme);
    token.line = _line;
    token.column = _column - static_cast<int>(token.lexeme.length()) + 1;
    out.push_back(token);
    return token;
}

Token Lexer::stringLiteral(std::vector<Token>& out) {
    // Remember the line where the opening quote lives so that the token's
    // reported line/column always point at the start of the literal, even
    // when the string spans multiple lines (L-H-02). Without this capture
    // a multi-line string would be reported on the *closing* line with the
    // column drifted by however many newlines we crossed.
    const int stringStartLine = _line;
    const int stringStartColumn = _column - 1;  // _column is 1 past opening "

    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') {
            _line++;
            _column = 0;
        }
        advance();
    }

    // L-H-03: an unterminated string used to emit `Unknown` with the leading
    // `"` baked into the lexeme (length = `_current - _start`, which
    // includes the opening quote). Downstream, that makes every error
    // message reference a phantom `"` character. Emit the lexeme WITHOUT
    // the opening quote so the diagnostic says "unterminated string"
    // rather than "weird quoted thing".
    if (isAtEnd()) {
        Token token;
        token.type = TokenType::Unknown;
        token.lexeme = _source.substr(_start + 1, _current - _start - 1);
        token.line = stringStartLine;
        token.column = stringStartColumn;
        out.push_back(token);
        return token;
    }

    advance();  // closing "
    Token token;
    token.type = TokenType::StringLiteral;
    // Lexeme carries the *raw* substring between the quotes (no escape
    // processing). Escape handling is the Parser's job.
    token.lexeme = _source.substr(_start + 1, _current - _start - 2);
    token.line = stringStartLine;
    token.column = stringStartColumn;
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

} // namespace ayt::shader::phoskia
