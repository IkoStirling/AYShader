// ============================================================
// Phoskia Phase 6 #6 — Fragment MRT (gl_FragData[N])
// ============================================================
//
// Fragment `out` targets (declaration order) lower to gl_FragData[0..N-1].
// Legacy single-color path keeps return → gl_FragColor when no outs.

#include "AYShader/Phoskia.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Lexer.h"
#include "AYShader/Parser.h"
#include "AYShader/Ast.h"
#include "AYShader/Ir.h"
#include "AYShader/SemanticAnalyzer.h"
#include "AYTest.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

TEST_SUITE(MRTFragmentTests)

static ayt::shader::detail::BGFXMaterialStages compileFirstMaterial(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ir::IRGenerator gen;
    auto ir = gen.generate(*ast);
    AYBGFXConverter conv;
    BGFXConvertResult res;
    conv.convertBGFX(ir, res);
    if (!res.success) {
        throw std::runtime_error("convertBGFX failed: " +
            (res.errors.empty() ? std::string("?") : res.errors.front()));
    }
    if (res.materialStages.empty()) {
        throw std::runtime_error("convertBGFX produced no materials");
    }
    return res.materialStages.front();
}

TEST_CASE(mrt_converter_emits_fragdata_slots) {
    const char* src = R"(
        material GBuffer {
            vertex {
                in pos : position
                return vec4(pos, 1.0)
            }
            fragment {
                in uv : texcoord
                out albedo : color = vec4(0.0)
                out nrm : color = vec4(0.0, 0.0, 1.0, 0.0)
                albedo = vec4(1.0, 0.0, 0.0, 1.0)
                nrm = vec4(0.0, 1.0, 0.0, 0.0)
            }
        }
    )";
    auto stages = compileFirstMaterial(src);
    CHECK(stages.fragment.find("gl_FragData[0]") != std::string::npos);
    CHECK(stages.fragment.find("gl_FragData[1]") != std::string::npos);
    CHECK(stages.fragment.find("gl_FragColor") == std::string::npos);
    CHECK(stages.fragment.find("gl_FragData[0] = ") != std::string::npos);
    CHECK(stages.fragment.find("gl_FragData[1] = ") != std::string::npos);
}

TEST_CASE(mrt_legacy_return_still_emits_fragcolor) {
    const char* src = R"(
        material Simple {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    auto stages = compileFirstMaterial(src);
    CHECK(stages.fragment.find("gl_FragColor") != std::string::npos);
    CHECK(stages.fragment.find("gl_FragData") == std::string::npos);
}

TEST_CASE(mrt_return_with_outs_is_semantic_error) {
    const char* src = R"(
        material Bad {
            vertex { return vec4(0.0) }
            fragment {
                out albedo : color = vec4(0.0)
                return vec4(1.0)
            }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();
    TypeEnvironment env;
    AYSemanticAnalyzer analyzer(env);
    bool ok = analyzer.analyze(*prog);
    CHECK(!ok);
    bool found = false;
    for (const auto& e : analyzer.errors()) {
        if (e.message.find("MRT") != std::string::npos ||
            e.message.find("cannot use 'return'") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE(mrt_ir_preserves_output_order) {
    const char* src = R"(
        material G {
            vertex { return vec4(0.0) }
            fragment {
                out a : color = vec4(0.0)
                out b : color = vec4(1.0)
                a = vec4(0.5)
                b = vec4(0.25)
            }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ir::IRGenerator gen;
    auto irProg = gen.generate(*ast);
    CHECK(!irProg.materials.empty());
    auto* mat = irProg.materials[0].get();
    CHECK(mat != nullptr);
    CHECK(mat->fragment != nullptr);
    CHECK(mat->fragment->outputs.size() == 2);
    auto* o0 = dynamic_cast<ir::IRShaderParam*>(mat->fragment->outputs[0].get());
    auto* o1 = dynamic_cast<ir::IRShaderParam*>(mat->fragment->outputs[1].get());
    CHECK(o0 != nullptr);
    CHECK(o1 != nullptr);
    if (o0) CHECK(o0->name == "a");
    if (o1) CHECK(o1->name == "b");
}

TEST_SUITE_END
