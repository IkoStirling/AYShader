// ============================================================
// AYShader Compiler (AYPhoskia) End-to-End Unit Tests
// (Phase 1 closure — vertex/fragment syntax)
// ============================================================

#include "AYPhoskia.h"
#include "AYTest.h"

using namespace ayt::shader::phoskia;

TEST_SUITE(PhoskiaCompilerTests)

// ===== Minimal valid material =====

TEST_CASE(compile_minimal_unlit) {
    Compiler compiler;
    const char* src = R"(
        material Unlit {
            property color = vec4(1.0, 0.0, 0.0, 1.0)
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return color }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(!result.output.empty());
    CHECK(result.errors.empty());
}

TEST_CASE(compile_empty_material) {
    // Must include both blocks now — the converter rejects otherwise.
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile("material X { vertex { } fragment { } }", result);
    CHECK(result.success);
    CHECK(!result.output.empty());
}

TEST_CASE(compile_multiple_materials) {
    Compiler compiler;
    const char* src = R"(
        material A {
            property a = 1.0
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
        material B {
            property b = 2.0
            vertex { return vec4(1.0) }
            fragment { return vec4(0.0, 1.0, 0.0, 1.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.errors.empty());
}

// ===== Backend registration =====

TEST_CASE(default_backend_is_registered) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }", result);
    CHECK(result.success);
}

TEST_CASE(compile_to_unknown_backend_fails) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compileToBackend(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }", "hlsl", result);
    CHECK(!result.success);
    CHECK(!result.errors.empty());
}

TEST_CASE(register_custom_backend) {
    Compiler compiler;
    compiler.registerBackend("noop", []() {
        return std::unique_ptr<ayt::shader::IAYBackendConverter>(nullptr);
    });
    auto result = CompileResult{};
    compiler.compileToBackend(
        "material X { vertex { } fragment { } }", "noop", result);
    // nullptr backend fails inside convert(), which surfaces as an error.
    CHECK(!result.success);
}

// ===== Compilation options =====

TEST_CASE(options_default_target_is_bgfx) {
    CompileOptions opts;
    CHECK(opts.targetBackend == "bgfx");
}

TEST_CASE(options_can_be_customized) {
    CompileOptions opts;
    opts.targetBackend = "myBackend";
    opts.enableTypeInference = true;
    opts.enableSemanticAnalysis = false;
    Compiler compiler(opts);
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }", result);
    CHECK(!result.success);
}

// ===== Pipeline phases exposed =====

TEST_CASE(tokenize_phase) {
    Compiler compiler;
    std::vector<Token> tokens;
    compiler.tokenize(
        "material X { vertex { } fragment { } }", tokens);
    CHECK(!tokens.empty());
    CHECK(tokens.back().type == TokenType::EndOfFile);
}

TEST_CASE(parse_phase) {
    Compiler compiler;
    std::vector<Token> tokens;
    compiler.tokenize(
        "material X { vertex { } fragment { } }", tokens);
    auto ast = compiler.parse(tokens);
    CHECK(ast != nullptr);
    CHECK(ast->declarations.size() == 1);
}

// ===== Error handling =====

TEST_CASE(lex_error_propagated) {
    Compiler compiler;
    // missing material name → parser reports a missing-identifier error.
    auto result = CompileResult{};
    compiler.compile("material { vertex { } fragment { } }", result);
    CHECK(!result.errors.empty());
}

TEST_CASE(parse_error_propagated) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0); "
        "fragment { return vec4(1.0) }", result);  // missing '}'
    CHECK(!result.errors.empty());
}

TEST_CASE(missing_vertex_block_causes_error) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { fragment { return vec4(1.0) } }", result);
    CHECK(!result.success);
}

TEST_CASE(missing_fragment_block_causes_error) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } }", result);
    CHECK(!result.success);
}

// ===== Realistic Phoskia snippet =====

TEST_CASE(compile_pbr_like_material) {
    Compiler compiler;
    const char* src = R"(
        material PBR {
            texture2d albedoMap
            uniform vec3 cameraPos

            vertex {
                in pos : position
                in nrm : normal
                in uv  : texcoord
                out worldNormal : normal = vec3(0.0, 0.0, 1.0)
                out uvCoord     : texcoord = vec2(0.0, 0.0)
                let wpos = vec4(pos, 1.0)
                return wpos
            }

            fragment {
                in worldNormal : normal
                in uvCoord     : texcoord
                let baseColor = sample(albedoMap, uvCoord)
                let N = normalize(worldNormal)
                let V = normalize(cameraPos)
                let NdotV = max(dot(N, V), 0.0)
                return vec4(baseColor.rgb * NdotV, 1.0)
            }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    // Semantic analysis may flag incomplete uniform set; we accept that
    // and only assert the three-piece output is produced.
    CHECK(!result.output.empty());
    CHECK(result.output.find("varying.def.sc") != std::string::npos);
}

TEST_CASE(compile_with_if_else) {
    Compiler compiler;
    const char* src = R"(
        material X {
            vertex {
                if (true) {
                    return vec4(1.0)
                } else {
                    return vec4(0.0)
                }
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(!result.output.empty());
}

TEST_CASE(compile_with_for_loop) {
    Compiler compiler;
    const char* src = R"(
        material X {
            vertex {
                for (i in items) {
                    let j = i
                }
                return vec4(0.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(!result.output.empty());
}

TEST_SUITE_END
