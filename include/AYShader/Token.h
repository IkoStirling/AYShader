#pragma once
// AYShader\Token.h - Token definitions for Phoskia lexer
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

// Token types.
//
// IMPORTANT: TokenType values cross translation-unit boundaries through the
// AST.  Keep the existing ordinal values stable and append new entries only;
// inserting an entry in the middle can make a stale incremental-build object
// interpret an operator as a different token.
enum class TokenType : uint8_t {
    // Keywords
    Material,
    Property,
    Uniform,
    Storage,
    Shared,
    UniformBlock,
    Binding,   // Phase 3.5-A: storage decl binding syntax
               //   storage NAME : rwstructuredbuffer<T> binding N;
    Texture2D,
    TextureCube,  // Phase 5 slice: texturecube envMap;
    Sampler,
    Vertex,
    Fragment,
    Compute,
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

    // Phase 1 RD-03: skeletal skinning vertex attributes.
    // Map to bgfx::Attrib::Indices (4x u8 integer, not normalized) and
    // bgfx::Attrib::Weight (4x f32) via the BGFX converter.
    BoneIndices,
    BoneWeights,
    Tangent,

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
    // Catch-all for unrecognized character runs. Phoskia doesn't use `#`;
    // the lexer silently consumes it (see `case '#'` in AYLexer.cpp).
    Unknown,

    // Fragment-only control-flow statement. Appended to preserve every
    // existing TokenType ordinal across incremental/stale-object builds.
    Discard
};

// ABI sentinels for the AST/operator boundary.  These deliberately fail the
// build if a future token is inserted before the existing operator or tail
// ranges. Append new tokens after Unknown.
static_assert(static_cast<uint8_t>(TokenType::Plus) == 30);
static_assert(static_cast<uint8_t>(TokenType::Unknown) == 60);
static_assert(static_cast<uint8_t>(TokenType::Discard) == 61);

struct Token {
    TokenType type = TokenType::Unknown;
    std::string lexeme;
    int line = 0;
    int column = 0;
};

} // namespace ayt::shader::phoskia
