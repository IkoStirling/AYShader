#pragma once
// AYBuiltinTypes.h - Phoskia builtin type table
//
// Phase 2 Step 5: The Lexer used to recognize "float", "vec2", "vec3",
// "vec4", "int", "ivec2..4", "mat2..4", "quat", "bool" as dedicated
// keyword tokens. After the token-demotion refactor those names are
// plain Identifier tokens again — to know whether one of those
// identifiers is a *builtin type* (rather than a user variable named
// "vec3") the parser / semantic analyzer consult this table.
//
// `isBuiltinType` takes a string_view so callers can pass either a
// std::string lexeme or a Token.lexeme without allocating. The lookup
// is hash-based (unordered_set<string_view>) and effectively O(1).

#include <string_view>
#include <string>
#include <unordered_set>

namespace ayt::shader::phoskia
{

class AYBuiltinTypes {
public:
    // Returns true if `lexeme` is one of the Phoskia builtin type
    // names: float / vec2 / vec3 / vec4 / int / ivec2 / ivec3 / ivec4 /
    // mat2 / mat3 / mat4 / quat / bool.
    static bool isBuiltinType(std::string_view lexeme);

    // Returns the full set of builtin names (for diagnostics, error
    // messages, and any future tooling that needs to enumerate them).
    static const std::unordered_set<std::string_view>& all();

    // Convenience: returns the comma-separated list of builtin names
    // suitable for embedding in a parser / semantic error message.
    // Example: "expected: float, vec2, vec3, ...".
    static std::string expectedList();
};

} // namespace ayt::shader::phoskia