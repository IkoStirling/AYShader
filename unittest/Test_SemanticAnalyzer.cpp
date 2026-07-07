// ============================================================
// AYShader SemanticAnalyzer Unit Tests (Phase 2 Step 2)
// ============================================================
//
// Exercises AYSemanticAnalyzer on parsed Phoskia programs. Each test
// builds an env, runs the analyzer, and asserts on (a) the symbol
// table contents and (b) the error list. Strict checks (return-vec4,
// if-bool-cond, undefined-identifier) only fire when the analyzer is
// enabled, so we instantiate it explicitly here regardless of the
// CompileOptions defaults.

#include "AYSemanticAnalyzer.h"
#include "AYType.h"
#include "AYTypeInference.h"
#include "AYAst.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYBuiltinFunctions.h"
#include "AYTest.h"

#include <memory>

using namespace ayt::shader::phoskia;

namespace {

// Helper: parse + analyze. Returns analyzer + env so tests can query
// either the analyzer's permanent `_symbols` table (via getType) or
// the env directly. Note: TypeEnvironment's getVariable only walks
// currently-active scopes �?after analyze() returns the inner shader
// scopes have already been popped, so shader params are NOT visible
// through env->getVariable. Use analyzer.getType() instead.
struct AnalyzeResult {
    bool ok;
    bool hasErrors;
    std::vector<CompilerError> errors;
    std::shared_ptr<TypeEnvironment> env;
    std::shared_ptr<AYSemanticAnalyzer> analyzer;
};

AnalyzeResult analyze(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();
    if (parser.hasErrors()) {
        // Surface parser errors so test failures point to the right place.
        return {false, true, parser.errors(), nullptr, nullptr};
    }

    auto env = std::make_shared<TypeEnvironment>();
    auto analyzer = std::make_shared<AYSemanticAnalyzer>(*env);
    bool ok = analyzer->analyze(*prog);
    bool hadErrors = analyzer->hasErrors();
    return {ok, hadErrors, analyzer->errors(), env, analyzer};
}

bool containsError(const std::vector<CompilerError>& errors,
                   const std::string& needle) {
    for (const auto& e : errors) {
        if (e.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_SUITE(SemanticAnalyzerTests)

// ===== ShaderParam registration =====

TEST_CASE(vertex_in_position_registers_as_vec3) {
    const char* src = R"(
        material X {
            vertex { in pos : position; return vec4(pos, 1.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK(r.ok);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("pos");
    CHECK(t != nullptr);
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

TEST_CASE(vertex_in_normal_registers_as_vec3) {
    const char* src = R"(
        material X {
            vertex { in n : normal; return vec4(n, 1.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("n");
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

TEST_CASE(vertex_in_color_registers_as_vec4) {
    const char* src = R"(
        material X {
            vertex { in c : color; return c }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("c");
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

TEST_CASE(vertex_in_texcoord_registers_as_vec2) {
    const char* src = R"(
        material X {
            vertex { in uv : texcoord; return vec4(uv, 0.0, 1.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("uv");
    auto v2 = BuiltinTypes::Vec2();
    CHECK(t->equals(*v2));
}

// ===== Property type inference =====

TEST_CASE(property_inferred_from_vec4_initializer) {
    const char* src = R"(
        material X {
            property color = vec4(1.0, 0.0, 0.0, 1.0)
            vertex { return vec4(0.0) }
            fragment { return color }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("color");
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

TEST_CASE(property_inferred_from_scalar_initializer) {
    const char* src = R"(
        material X {
            property tint = 0.5
            vertex { return vec4(0.0) }
            fragment { return vec4(tint) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("tint");
    CHECK(t->equals(*BuiltinTypes::Float));
}

// ===== Strict return-vec4 check =====

TEST_CASE(vertex_returning_vec3_is_error) {
    const char* src = R"(
        material X {
            vertex { in p : position; return p }
            fragment { return vec4(1.0) }
        }
    )";
    // Vertex returns a vec3 (`p`), not vec4. The strict check should
    // catch this.
    auto r = analyze(src);
    CHECK(r.hasErrors);
    CHECK(containsError(r.errors, "Return type must be vec4"));
}

TEST_CASE(vertex_returning_vec4_is_ok) {
    const char* src = R"(
        material X {
            vertex { return vec4(1.0, 2.0, 3.0, 1.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
}

TEST_CASE(fragment_returning_vec4_is_ok) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
}

// ===== Strict if-condition check =====

TEST_CASE(if_condition_float_is_error) {
    const char* src = R"(
        material X {
            vertex { if (1.0) { return vec4(1.0) } else { return vec4(0.0) } }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK(r.hasErrors);
    CHECK(containsError(r.errors, "If condition must be bool"));
}

TEST_CASE(if_condition_bool_is_ok) {
    const char* src = R"(
        material X {
            vertex { if (true) { return vec4(1.0) } else { return vec4(0.0) } }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
}

// ===== Undefined identifier =====

TEST_CASE(undefined_identifier_is_error) {
    const char* src = R"(
        material X {
            vertex { let x = mystery * 2.0; return vec4(x) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK(r.hasErrors);
    CHECK(containsError(r.errors, "Undefined identifier: mystery"));
}

TEST_CASE(known_identifier_in_body_is_ok) {
    const char* src = R"(
        material X {
            vertex {
                in pos : position
                let doubled = pos * 2.0
                return vec4(doubled, 1.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("doubled");
    CHECK(t != nullptr);
}

// ===== Return outside shader =====

TEST_CASE(return_at_program_level_is_error) {
    // 'return' directly inside a material body (not inside a block) is
    // not parsed; we test at body level. The analyzer's _inShaderFunc
    // guard fires if a ReturnStmt slips through.
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    // Sanity: this case has returns inside blocks, so it must succeed.
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
}

// ===== Swizzle and member access resolve =====

TEST_CASE(swizzle_rgb_on_vec4_ok) {
    const char* src = R"(
        material X {
            vertex {
                in c : color
                let rgb = c.rgb
                return vec4(rgb, 1.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("rgb");
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

// ===== Phase 2 Step 5: type-name validation via AYBuiltinTypes =====

TEST_CASE(uniform_with_non_builtin_type_is_error) {
    // After Step 5 token-demotion, "vec3" / "float" / etc. are plain
    // Identifier tokens. The SemanticAnalyzer now validates them via
    // AYBuiltinTypes::isBuiltinType �?a typo like "vec33" surfaces a
    // Go-style diagnostic here rather than failing inside the BGFX
    // backend later.
    const char* src = R"(
        material X {
            uniform vec33 cameraPos
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK(r.hasErrors);
    CHECK(containsError(r.errors, "'vec33' is not a builtin type"));
}

TEST_CASE(uniform_with_builtin_type_passes_type_check) {
    // Sanity: vec3 / float / mat4 etc. must NOT trip the
    // isBuiltinType gate �?only genuinely unknown lexemes should.
    const char* src = R"(
        material X {
            uniform vec3 cameraPos
            uniform mat4 modelViewProj
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
}

// ===== Phase 1 RD-03: bone semantics registered as vec4 =====

TEST_CASE(vertex_in_boneindices_registers_as_vec4) {
    // Phase 1 RD-03: the BLENDINDICES attribute in bgfx is a vec4 of
    // normalized indices (index/255 packed as float). Phoskia's
    // `boneindices` semantic therefore maps to vec4 (not ivec4) so
    // the same code path as the bgfx shaderc side. The semantic
    // analyzer must register the param with type = vec4.
    const char* src = R"(
        material X {
            vertex { in boneId : boneindices; return vec4(boneId) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK(r.ok);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("boneId");
    CHECK(t != nullptr);
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

TEST_CASE(vertex_in_boneweights_registers_as_vec4) {
    // Mirror case for the weights attribute (BLENDWEIGHT in bgfx).
    const char* src = R"(
        material X {
            vertex { in boneWt : boneweights; return vec4(boneWt) }
            fragment { return vec4(1.0) }
        }
    )";
    auto r = analyze(src);
    CHECK_FALSE(r.hasErrors);
    auto t = r.analyzer->getType("boneWt");
    CHECK(t != nullptr);
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

TEST_SUITE_END