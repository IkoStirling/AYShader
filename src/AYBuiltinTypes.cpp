// AYBuiltinTypes.cpp - Phoskia builtin type table implementation

#include "AYBuiltinTypes.h"
#include <string>

namespace ayt::shader::phoskia
{

namespace {
// string_view-based set so callers can pass std::string lexemes
// without an extra allocation per lookup. The set is built once at
// program startup via the static initializer below.
const std::unordered_set<std::string_view>& builtinSet() {
    static const std::unordered_set<std::string_view> kSet = {
        "float",
        "vec2",  "vec3",  "vec4",
        "int",
        "uint",   // Phase 3.3 Block 1 — GLSL unsigned int (32-bit)
        "ivec2", "ivec3", "ivec4",
        "mat2",  "mat3",  "mat4",
        "quat",
        "bool",
    };
    return kSet;
}
}  // namespace

bool AYBuiltinTypes::isBuiltinType(std::string_view lexeme) {
    return builtinSet().find(lexeme) != builtinSet().end();
}

const std::unordered_set<std::string_view>& AYBuiltinTypes::all() {
    return builtinSet();
}

std::string AYBuiltinTypes::expectedList() {
    // Stable ordering so test assertions on error strings are
    // deterministic across runs.
    return "float, vec2, vec3, vec4, int, uint, ivec2, ivec3, ivec4, "
           "mat2, mat3, mat4, quat, bool";
}

} // namespace ayt::shader::phoskia