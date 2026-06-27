#pragma once
// AYToken.h - Token definitions for Phoskia lexer
//
// A Token is a purely lexical artifact: it carries the raw text slice
// (`lexeme`) and its source position. Conversion to typed literals
// (float / int / string) is the Parser's responsibility — keeping the
// lexer free of value-level types preserves the layering and avoids the
// MSVC SSO string-move hazards that appear when a std::variant holding
// std::string is moved inside vectors of tokens.

#include <cstdint>
#include <string>

namespace ayt::shader::phoskia
{

// Token types
enum class TokenType : uint8_t {
    // Keywords
    Material,
    Property,
    Uniform,
    Texture2D,
    Sampler,
    Vertex,
    Fragment,
    Let,
    If,
    Else,
    For,
    In,
    Out,
    Return,
    True,
    False,
    Variant,

    // Phoskia semantic types (replaces bgfx POSITION/NORMAL/COLOR0/TEXCOORD0
    // for in/out parameter declarations inside vertex/fragment blocks).
    Position,
    Normal,
    Color,
    Texcoord,

    // Types
    Float,
    Vec2,
    Vec3,
    Vec4,
    Int,
    IVec2,
    IVec3,
    IVec4,
    Mat2,
    Mat3,
    Mat4,
    Quat,
    Bool,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,

    // Special tokens
    Dot,
    Comma,
    Colon,
    Semicolon,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,

    // Literals
    Identifier,
    FloatLiteral,
    IntLiteral,
    StringLiteral,

    // Special
    EndOfFile,
    Unknown
};

struct Token {
    TokenType type = TokenType::Unknown;
    std::string lexeme;
    int line = 0;
    int column = 0;
};

} // namespace ayt::shader::phoskia
