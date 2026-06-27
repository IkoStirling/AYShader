// ============================================================
// AYShader BGFX Backend Converter Unit Tests (Phase 1 closure)
// ============================================================
//
// Tests AYBGFXConverter::convertMaterial(), which emits the three-piece
// output a frontend feeds to bgfx shaderc: vs_*.sc, fs_*.sc, and the
// shared varying.def.sc.

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYTest.h"
#include <stdexcept>

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

TEST_SUITE(BGFXConverterTests)

// ===== Helpers =====

// End-to-end: source → AST → first material's three-piece set.
static BGFXShaderFiles compileFirstMaterial(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    AYBGFXConverter conv;
    BGFXConvertResult res = conv.convertBGFX(*ast);
    if (!res.success) {
        throw std::runtime_error("convertBGFX failed: " +
            (res.errors.empty() ? std::string("?") : res.errors.front()));
    }
    if (res.materialFiles.empty()) {
        throw std::runtime_error("convertBGFX produced no materials");
    }
    return res.materialFiles.front();
}

static std::unique_ptr<Program> parseProgram(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    return parser.parse();
}

// ===== Platform / extension =====

TEST_CASE(platform_is_bgfx) {
    AYBGFXConverter conv;
    CHECK(conv.targetPlatform() == Platform::BGFX);
    CHECK(std::string(conv.targetExtension()) == ".sc");
}

// ===== Three-piece set: empty material =====

TEST_CASE(empty_material_produces_three_pieces) {
    const char* src = R"(
        material Unlit {
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    auto files = compileFirstMaterial(src);
    // No in/out declarations on either block → no attributes or varyings
    // need to be registered in varying.def.sc. The file is still emitted
    // (empty string) so downstream shaderc invocations are deterministic.
    CHECK(!files.vs.empty());
    CHECK(!files.fs.empty());
    CHECK(files.varyingDef.empty());
}

// ===== vs content: $input / $output / uniforms / body =====

TEST_CASE(vertex_emits_input_output) {
    const char* src = R"(
        material X {
            vertex {
                in  pos : position
                in  nrm : normal
                out clr : color = vec4(1.0, 0.0, 0.0, 1.0)
                return vec4(pos, 1.0)
            }
            fragment { in clr : color; return clr }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vs.find("$input a_position, a_normal") != std::string::npos);
    CHECK(files.vs.find("$output v_color0") != std::string::npos);
    CHECK(files.vs.find("#include \"common.sh\"") != std::string::npos);
    CHECK(files.vs.find("gl_Position = vec4(a_position, 1.0)") != std::string::npos);
}

// ===== fs content: $input / uniforms / textures / body =====

TEST_CASE(fragment_emits_input_textures) {
    const char* src = R"(
        material PBR {
            texture2d albedoMap
            vertex {
                in pos : position
                in uv  : texcoord
                return vec4(pos, 1.0)
            }
            fragment {
                in uv : texcoord
                return texture2D(albedoMap, uv)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fs.find("$input v_texcoord0") != std::string::npos);
    CHECK(files.fs.find("SAMPLER2D(albedoMap, 0)") != std::string::npos);
    CHECK(files.fs.find("texture2D(albedoMap, v_texcoord0)") != std::string::npos);
}

// ===== varying.def.sc content =====

TEST_CASE(varying_def_emits_all_bindings) {
    const char* src = R"(
        material X {
            vertex {
                in  pos : position
                in  nrm : normal
                in  uv  : texcoord
                out clr : color = vec4(1.0, 0.0, 0.0, 1.0)
                out uv2 : texcoord = vec2(0.0, 0.0)
                return vec4(pos, 1.0)
            }
            fragment {
                in clr : color
                in uv2 : texcoord
                return clr
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    // Attributes
    CHECK(files.varyingDef.find("a_position  : POSITION") != std::string::npos);
    CHECK(files.varyingDef.find("a_normal    : NORMAL") != std::string::npos);
    CHECK(files.varyingDef.find("a_texcoord0 : TEXCOORD0") != std::string::npos);
    // Varyings (with default values from the table).
    CHECK(files.varyingDef.find("v_color0    : COLOR0    = vec4(1.0, 0.0, 0.0, 1.0)") != std::string::npos);
    CHECK(files.varyingDef.find("v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0)") != std::string::npos);
}

// ===== Uniforms =====

TEST_CASE(uniforms_emitted_in_both_vs_and_fs) {
    const char* src = R"(
        material X {
            uniform vec4 u_time
            uniform mat4 u_modelViewProj
            vertex {
                return u_modelViewProj * vec4(0.0, 0.0, 0.0, 1.0)
            }
            fragment {
                return vec4(u_time)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vs.find("uniform vec4 u_time") != std::string::npos);
    CHECK(files.vs.find("uniform mat4 u_modelViewProj") != std::string::npos);
    CHECK(files.fs.find("uniform vec4 u_time") != std::string::npos);
}

// ===== Properties become uniforms =====

TEST_CASE(properties_become_uniforms) {
    const char* src = R"(
        material X {
            property tint = vec4(1.0, 0.5, 0.25, 1.0)
            vertex { return vec4(0.0) }
            fragment { return tint }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fs.find("uniform vec4 tint = vec4(1.0, 0.5, 0.25, 1.0)") != std::string::npos);
}

// ===== builtin: sample() → texture2D() =====

TEST_CASE(sample_builtin_maps_to_texture2D) {
    const char* src = R"(
        material X {
            texture2d tex
            vertex { return vec4(0.0) }
            fragment {
                in uv : texcoord
                return sample(tex, uv)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fs.find("texture2D(tex, v_texcoord0)") != std::string::npos);
    CHECK(files.fs.find("sample(") == std::string::npos);
}

// ===== Structural validation: missing vertex/fragment surfaces as error =====
//
// In Phase 1 closure the parser rejects a material missing vertex or
// fragment at parse time (parseMaterialDecl records an error). The
// converter therefore never sees an incomplete material — but we keep
// a converter-level guard for direct AST injection paths (Phase 2
// compute-only materials). Replaced by parser-level error checks above.

TEST_CASE(compute_throws_not_implemented) {
    // Phase 1: with parser-level structural validation, a program
    // lacking a vertex/fragment fails to parse cleanly. The compiler
    // pipeline surfaces that as a parser error and never reaches the
    // converter. We assert the failure mode end-to-end here.
    Lexer lexer("material X { fragment { return vec4(1.0) } }");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    CHECK(parser.hasErrors());
    // Even if the AST is partial, the converter must still refuse to
    // emit silently — a malformed material cannot yield valid bgfx code.
    AYBGFXConverter conv;
    auto res = conv.convertBGFX(*ast);
    CHECK(!res.success);
    CHECK(!res.errors.empty());
}

// ===== Multiple materials =====

TEST_CASE(multiple_materials_each_get_three_pieces) {
    const char* src = R"(
        material A {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
        material B {
            vertex { return vec4(1.0) }
            fragment { return vec4(0.0, 1.0, 0.0, 1.0) }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    AYBGFXConverter conv;
    auto res = conv.convertBGFX(*ast);
    CHECK(res.success);
    CHECK(res.materialFiles.size() == 2);
    CHECK(!res.materialFiles[0].vs.empty());
    CHECK(!res.materialFiles[1].vs.empty());
    // First material's fs should reflect its color (red), second (green).
    CHECK(res.materialFiles[0].fs.find("1.0, 0.0, 0.0, 1.0") != std::string::npos);
    CHECK(res.materialFiles[1].fs.find("0.0, 1.0, 0.0, 1.0") != std::string::npos);
}

// ===== End-to-end via Compiler =====

TEST_CASE(compiler_emits_three_pieces) {
    ayt::shader::phoskia::Compiler compiler;
    const char* src = R"(
        material Unlit {
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    auto result = compiler.compile(src);
    CHECK(result.success);
    // The generic .output is a concatenated stream of the three files
    // with fences; the structured form lives in the per-material uniforms/textures.
    CHECK(!result.output.empty());
    CHECK(result.output.find("varying.def.sc") != std::string::npos);
    CHECK(result.output.find("$input") != std::string::npos);
}

TEST_SUITE_END
