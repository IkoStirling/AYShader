// Test_Phase5Slice.cpp — Phase 5 small-step: derivatives + texturecube

#include "AYShader/Phoskia.h"
#include "AYTest.h"

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

CompiledShaderProgram compileWithSources(const std::string& src)
{
    Compiler compiler;
    CompileOptions opts;
    opts.keepSources = true;
    return compiler.compileToProgram(src, opts);
}

} // namespace

TEST_SUITE(Phase5SliceTests)

TEST_CASE(lexer_recognizes_texturecube_keyword)
{
    Lexer lexer("texturecube envMap");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() >= 2);
    CHECK(tokens[0].type == TokenType::TextureCube);
    CHECK(tokens[0].lexeme == "texturecube");
}

TEST_CASE(fragment_derivatives_emit_glsl_names)
{
    CompiledShaderProgram prog = compileWithSources(R"(
        material M {
            vertex {
                in nrm : normal
                out nrmOut : normal
                return vec4(nrm, 1.0)
            }
            fragment {
                in nrmOut : normal
                let dx = dFdx(nrmOut.x)
                let dy = dFdy(nrmOut.y)
                let fw = fwidth(nrmOut.z)
                return vec4(dx, dy, fw, 1.0)
            }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& fs = prog.sources.at("fragment_stage_0");
    CHECK(fs.find("dFdx(") != std::string::npos);
    CHECK(fs.find("dFdy(") != std::string::npos);
    CHECK(fs.find("fwidth(") != std::string::npos);
}

TEST_CASE(texturecube_emits_samplercube_and_textureCube)
{
    CompiledShaderProgram prog = compileWithSources(R"(
        material IBL {
            texturecube envMap
            vertex {
                in nrm : normal
                out nrmOut : normal
                return vec4(nrm, 1.0)
            }
            fragment {
                in nrmOut : normal
                let dir = normalize(nrmOut)
                let rgb = sample(envMap, dir)
                return vec4(rgb.rgb, 1.0)
            }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& fs = prog.sources.at("fragment_stage_0");
    CHECK(fs.find("SAMPLERCUBE(envMap, 0)") != std::string::npos);
    CHECK(fs.find("textureCube(envMap, dir)") != std::string::npos);
}

TEST_CASE(texturecube_explicit_lod_emits_textureCubeLod)
{
    CompiledShaderProgram prog = compileWithSources(R"(
        material IBLPrefilter {
            texturecube prefilteredEnv
            vertex {
                in nrm : normal
                out nrmOut : normal
                return vec4(nrm, 1.0)
            }
            fragment {
                in nrmOut : normal
                let dir = normalize(nrmOut)
                let rgb = sampleLod(prefilteredEnv, dir, 3.0)
                return vec4(rgb.rgb, 1.0)
            }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& fs = prog.sources.at("fragment_stage_0");
    CHECK(fs.find("SAMPLERCUBE(prefilteredEnv, 0)") != std::string::npos);
    CHECK(fs.find("textureCubeLod(prefilteredEnv, dir, 3.0)")
          != std::string::npos);
}

TEST_SUITE_END
