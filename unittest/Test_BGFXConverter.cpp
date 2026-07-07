// ============================================================
// AYShader BGFX Backend Converter Unit Tests (Phase 1 closure)
// ============================================================
//
// Tests AYBGFXConverter::convertMaterial(), which emits the three-piece
// output a frontend feeds to bgfx shaderc: vs_*.sc, fs_*.sc, and the
// shared varying_definitions.

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include "detail/AYPhoskiaFrameBuiltins.h"
#include "detail/AYBGFXStageSources.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYIr.h"
#include "AYShadercDriver.h"
#include "AYTest.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_set>

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

TEST_SUITE(BGFXConverterTests)

// ===== Helpers =====

static ayt::shader::detail::BGFXMaterialStages compileFirstMaterial(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ir::IRGenerator gen;
    auto ir = gen.generate(*ast);
    AYBGFXConverter conv;
    // Phase 3.2-pre SSO/NRVO fix: use the out-param form.
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

#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif
#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT "../../../thirdparty/bgfx/examples/common"
#endif
#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT "../../../thirdparty/bgfx/src"
#endif

static bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

static bool shadercEnvironmentAvailable()
{
    if (!fileExists(AY_SHADER_SHADERC_HINT) || !fileExists(AY_SHADER_BGFX_COMMON_HINT)) {
        return false;
    }
    try {
        AYShadercDriver probe(AY_SHADER_SHADERC_HINT);
        return !probe.shadercPath().empty();
    } catch (...) {
        return false;
    }
}

static std::vector<std::string> bgfxShaderIncludeDirs()
{
    std::vector<std::string> dirs;
    if (fileExists(AY_SHADER_BGFX_COMMON_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_COMMON_HINT);
    }
    if (fileExists(AY_SHADER_BGFX_SRC_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_SRC_HINT);
    }
    return dirs;
}

static bool varyingDefinitionsHaveUniqueInterpolatorSemantics(const std::string& vdef)
{
    std::unordered_set<std::string> seen;
    std::istringstream lines(vdef);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t namePos = line.find("v_");
        if (namePos == std::string::npos) {
            continue;
        }
        const size_t colonPos = line.find(':', namePos);
        if (colonPos == std::string::npos) {
            continue;
        }
        const size_t eqPos = line.find('=', colonPos);
        const size_t endPos = eqPos == std::string::npos ? line.size() : eqPos;
        std::string semantic = line.substr(colonPos + 1, endPos - colonPos - 1);
        while (!semantic.empty() && semantic.front() == ' ') {
            semantic.erase(semantic.begin());
        }
        while (!semantic.empty() && semantic.back() == ' ') {
            semantic.pop_back();
        }
        if (semantic.empty() || !seen.insert(semantic).second) {
            return false;
        }
    }
    return true;
}

static bool compileMaterialStagesForProfile(const ayt::shader::detail::BGFXMaterialStages& files,
                                            const std::string& platform,
                                            const std::string& profile)
{
    AYShadercDriver driver(AY_SHADER_SHADERC_HINT);
    const std::vector<std::string> includeDirs = bgfxShaderIncludeDirs();

    auto compileStage = [&](const char* stage, const std::string& source) {
        ShaderCompileRequest req;
        req.scSource = source;
        req.stage = stage;
        req.varyingdefSource = files.varyingDefinitions;
        req.platform = platform;
        req.profile = profile;
        req.includeDirs = includeDirs;
        req.outputName = std::string(stage) + "_" + platform + "_" + profile;
        const ShaderCompileResult result = driver.compile(req);
        if (!result.ok) {
            std::fprintf(stderr,
                         "shaderc failed (%s/%s %s): %s\n",
                         platform.c_str(), profile.c_str(), stage,
                         result.stderrText.c_str());
        }
        return result.ok;
    };

    return compileStage("vertex", files.vertex)
        && compileStage("fragment", files.fragment);
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
    // No in/out declarations on either block ?no attributes or varyings
    // need to be registered in varying_definitions. The file is still emitted
    // (empty string) so downstream shaderc invocations are deterministic.
    CHECK(!files.vertex.empty());
    CHECK(!files.fragment.empty());
    CHECK(files.varyingDefinitions.empty());
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
    CHECK(files.vertex.find("$input a_position, a_normal") != std::string::npos);
    CHECK(files.vertex.find("$output v_color0") != std::string::npos);
    CHECK(files.vertex.find("#include \"common.sh\"") != std::string::npos);
    CHECK(files.vertex.find("gl_Position = vec4(a_position, 1.0)") != std::string::npos);
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
    CHECK(files.fragment.find("$input v_texcoord0") != std::string::npos);
    CHECK(files.fragment.find("SAMPLER2D(albedoMap, 0)") != std::string::npos);
    CHECK(files.fragment.find("texture2D(albedoMap, v_texcoord0)") != std::string::npos);
}

// ===== varying_definitions content =====

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
    CHECK(files.varyingDefinitions.find("a_position  : POSITION") != std::string::npos);
    CHECK(files.varyingDefinitions.find("a_normal    : NORMAL") != std::string::npos);
    CHECK(files.varyingDefinitions.find("a_texcoord0 : TEXCOORD0") != std::string::npos);
    // Varyings (with default values from the table).
    CHECK(files.varyingDefinitions.find("v_color0    : COLOR0    = vec4(1.0, 0.0, 0.0, 1.0)") != std::string::npos);
    CHECK(files.varyingDefinitions.find("v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0)") != std::string::npos);
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
    CHECK(files.vertex.find("uniform vec4 u_time") != std::string::npos);
    CHECK(files.vertex.find("uniform mat4 u_modelViewProj") != std::string::npos);
    CHECK(files.fragment.find("uniform vec4 u_time") != std::string::npos);
    CHECK(files.vertex.find("mul(u_modelViewProj, vec4(0.0, 0.0, 0.0, 1.0))") != std::string::npos);
}

TEST_CASE(phoskia_frame_builtins_lower_to_bgfx_common_sh) {
    const char* src = R"(
        material Lit {
            vertex {
                in pos : position
                return modelViewProjection * vec4(pos, 1.0)
            }
            fragment {
                return vec4(1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vertex.find("modelViewProjection") == std::string::npos);
    CHECK(files.vertex.find("mul(u_modelViewProj, vec4(a_position, 1.0))") != std::string::npos);
}

TEST_CASE(phoskia_model_matrix_uses_mul_in_out_default) {
    const char* src = R"(
        material X {
            vertex {
                in nrm : normal
                out worldNormal : normal = (modelMatrix * vec4(nrm, 0.0)).xyz
                return modelViewProjection * vec4(0.0, 0.0, 0.0, 1.0)
            }
            fragment {
                in worldNormal : normal
                return vec4(worldNormal, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vertex.find("modelMatrix") == std::string::npos);
    CHECK(files.vertex.find("mul(u_model[0], vec4(a_normal, 0.0))") != std::string::npos);
    CHECK(files.vertex.find("u_model[0] * float4") == std::string::npos);
}

TEST_CASE(phoskia_matrix_frame_builtins_lower_to_bgfx) {
    const char* src = R"(
        material X {
            vertex {
                in pos : position
                let w = viewProjectionMatrix * vec4(pos, 1.0)
                let v = viewMatrix * vec4(pos, 1.0)
                let p = projectionMatrix * vec4(pos, 1.0)
                let mv = modelViewMatrix * vec4(pos, 1.0)
                let invV = inverseViewMatrix * vec4(pos, 1.0)
                let invP = inverseProjectionMatrix * vec4(pos, 1.0)
                let invVP = inverseViewProjectionMatrix * vec4(pos, 1.0)
                let invMV = inverseModelViewMatrix * vec4(pos, 1.0)
                return modelViewProjection * vec4(pos, 1.0)
            }
            fragment {
                return vec4(1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vertex.find("inverseViewMatrix") == std::string::npos);
    CHECK(files.vertex.find("inverseProjectionMatrix") == std::string::npos);
    CHECK(files.vertex.find("inverseViewProjectionMatrix") == std::string::npos);
    CHECK(files.vertex.find("inverseModelViewMatrix") == std::string::npos);
    CHECK(files.vertex.find("mul(u_viewProj,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_view,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_proj,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_modelView,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_invView,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_invProj,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_invViewProj,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_invModelView,") != std::string::npos);
    CHECK(files.vertex.find("mul(u_modelViewProj,") != std::string::npos);
}

TEST_CASE(phoskia_viewport_and_alpha_frame_builtins_lower_in_fragment) {
    const char* src = R"(
        material X {
            vertex {
                return modelViewProjection * vec4(0.0, 0.0, 0.0, 1.0)
            }
            fragment {
                let rect = viewportRect
                let texel = viewportTexel
                let alpha = alphaReference
                return vec4(rect.xy * texel.xy, alpha, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("viewportRect") == std::string::npos);
    CHECK(files.fragment.find("viewportTexel") == std::string::npos);
    CHECK(files.fragment.find("alphaReference") == std::string::npos);
    CHECK(files.fragment.find("u_viewRect") != std::string::npos);
    CHECK(files.fragment.find("u_viewTexel") != std::string::npos);
    CHECK(files.fragment.find("u_alphaRef") != std::string::npos);
}

TEST_CASE(phoskia_frame_builtin_table_covers_bgfx_common_sh) {
    using namespace ayt::shader::phoskia::detail;
    CHECK(kFrameBuiltinCount == 13u);
    for (std::size_t i = 0; i < kFrameBuiltinCount; ++i) {
        const FrameBuiltinSpec& spec = kFrameBuiltins[i];
        CHECK(findFrameBuiltin(spec.phoskiaName) == &spec);
        CHECK(std::string(bgfxFrameBuiltinExpr(spec.phoskiaName)) == spec.bgfxExpr);
        if (spec.kind == FrameBuiltinKind::Mat4) {
            CHECK(frameBuiltinIsMatrix(spec.phoskiaName));
        } else {
            CHECK(!frameBuiltinIsMatrix(spec.phoskiaName));
        }
    }
}

TEST_CASE(lit_shader_varying_normal_and_uv_use_bgfx_cross_platform_semantics) {
    const char* src = R"(
        material Lit {
            vertex {
                in nrm : normal
                in uv  : texcoord
                out nOut : normal = nrm
                out uvOut : texcoord = uv
                return modelViewProjection * vec4(0.0, 0.0, 0.0, 1.0)
            }
            fragment {
                in nOut : normal
                in uvOut : texcoord
                return vec4(nOut, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.varyingDefinitions.find("v_texcoord0 : TEXCOORD0") != std::string::npos);
    CHECK(files.varyingDefinitions.find("v_normal    : NORMAL") != std::string::npos);
    CHECK(files.varyingDefinitions.find("a_normal    : NORMAL") != std::string::npos);
    CHECK(files.varyingDefinitions.find("a_texcoord0 : TEXCOORD0") != std::string::npos);
    CHECK(varyingDefinitionsHaveUniqueInterpolatorSemantics(files.varyingDefinitions));
}

TEST_CASE(lit_shader_varying_slots_texcoord_only) {
    const char* src = R"(
        material Unlit {
            vertex {
                in uv : texcoord
                out uvOut : texcoord = uv
                return modelViewProjection * vec4(0.0, 0.0, 0.0, 1.0)
            }
            fragment {
                in uvOut : texcoord
                return vec4(uvOut, 0.0, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.varyingDefinitions.find("v_texcoord0 : TEXCOORD0") != std::string::npos);
    CHECK(files.varyingDefinitions.find("v_normal") == std::string::npos);
    CHECK(varyingDefinitionsHaveUniqueInterpolatorSemantics(files.varyingDefinitions));
}

TEST_CASE(lit_shader_varying_slots_normal_texcoord_color) {
    const char* src = R"(
        material Lit {
            vertex {
                in nrm : normal
                in uv  : texcoord
                in clr : color
                out nOut : normal = nrm
                out uvOut : texcoord = uv
                out cOut : color = clr
                return modelViewProjection * vec4(0.0, 0.0, 0.0, 1.0)
            }
            fragment {
                in nOut : normal
                in uvOut : texcoord
                in cOut : color
                return cOut
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.varyingDefinitions.find("v_color0    : COLOR0") != std::string::npos);
    CHECK(files.varyingDefinitions.find("v_texcoord0 : TEXCOORD0") != std::string::npos);
    CHECK(files.varyingDefinitions.find("v_normal    : NORMAL") != std::string::npos);
    CHECK(varyingDefinitionsHaveUniqueInterpolatorSemantics(files.varyingDefinitions));
}

TEST_CASE(lit_shader_shaderc_across_bgfx_platform_profiles) {
    if (!shadercEnvironmentAvailable()) {
        std::fprintf(stderr, "SKIP lit_shader_shaderc_across_bgfx_platform_profiles: shaderc/common.sh unavailable\n");
        return;
    }

    const char* src = R"(
        material SimpleLit {
            texture2d albedoMap
            uniform vec3 lightDir
            property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
            vertex {
                in pos : position
                in nrm : normal
                in uv  : texcoord
                out worldNormal : normal = (modelMatrix * vec4(nrm, 0.0)).xyz
                out uvOut : texcoord = uv
                return modelViewProjection * vec4(pos, 1.0)
            }
            fragment {
                in worldNormal : normal
                in uvOut : texcoord
                let albedo = sample(albedoMap, uvOut) * baseColor
                let ndotl = max(dot(normalize(worldNormal), normalize(lightDir)), 0.05)
                return vec4(albedo.rgb * ndotl, albedo.a)
            }
        }
    )";
    const ayt::shader::detail::BGFXMaterialStages files = compileFirstMaterial(src);
    CHECK(varyingDefinitionsHaveUniqueInterpolatorSemantics(files.varyingDefinitions));

    struct ProfileTarget {
        const char* platform;
        const char* profile;
    };
    static const ProfileTarget kRequiredTargets[] = {
        {"windows", "s_5_0"},
        {"linux", "430"},
    };
    for (const ProfileTarget& target : kRequiredTargets) {
        CHECK(compileMaterialStagesForProfile(files, target.platform, target.profile));
    }
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
    CHECK(files.fragment.find("uniform vec4 tint = vec4(1.0, 0.5, 0.25, 1.0)") != std::string::npos);
}

// ===== builtin: sample() ?texture2D() =====

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
    CHECK(files.fragment.find("texture2D(tex, v_texcoord0)") != std::string::npos);
    CHECK(files.fragment.find("sample(") == std::string::npos);
}

// ===== Structural validation: missing vertex/fragment surfaces as error =====
//
// In Phase 1 closure the parser rejects a material missing vertex or
// fragment at parse time (parseMaterialDecl records an error). The
// converter therefore never sees an incomplete material ?but we keep
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
    // emit silently ?a malformed material cannot yield valid bgfx code.
    ir::IRGenerator gen;
    AYBGFXConverter conv;
    BGFXConvertResult res;
    conv.convertBGFX(gen.generate(*ast), res);
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
    ir::IRGenerator gen;
    AYBGFXConverter conv;
    BGFXConvertResult res;
    conv.convertBGFX(gen.generate(*ast), res);
    CHECK(res.success);
    CHECK(res.materialStages.size() == 2);
    CHECK(!res.materialStages[0].vertex.empty());
    CHECK(!res.materialStages[1].vertex.empty());
    // First material's fs should reflect its color (red), second (green).
    CHECK(res.materialStages[0].fragment.find("1.0, 0.0, 0.0, 1.0") != std::string::npos);
    CHECK(res.materialStages[1].fragment.find("0.0, 1.0, 0.0, 1.0") != std::string::npos);
}

// ===== End-to-end via Compiler =====

TEST_CASE(compiler_emits_three_pieces) {
    // Commit 5 emit-shape contract: three .sc pieces (vs, fs,
    // varying_definitions) with bgfx prologue ($input in vs).
    //
    // Use convertBGFX (same as the rest of this file). Do NOT call
    // compileToProgram here — that path also invokes shaderc when a
    // process default is configured, which is unrelated to the shape
    // this case locks. compileToProgram + keepSources is covered by
    // Test_CompileToProgram.cpp; shaderc round-trip by
    // lit_shader_shaderc_across_bgfx_platform_profiles above.
    const char* src = R"(
        material Unlit {
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    const ayt::shader::detail::BGFXMaterialStages files = compileFirstMaterial(src);
    // The empty Unlit material has no `in` decls, so the
    // varying_definitions piece is legitimately empty (see
    // empty_material_produces_three_pieces above). The vs/fs pieces
    // must still be populated and the vs must carry the bgfx $input
    // prologue so the frontend can hand it to shaderc verbatim.
    CHECK(!files.vertex.empty());
    CHECK(!files.fragment.empty());
    CHECK(files.vertex.find("$input") != std::string::npos);
}

// ===== Phase 2 Step 1: [variant] expands to opt-in #ifndef =====
TEST_CASE(variant_expands_to_ifndef_in_fragment) {
    const char* src = R"(
        material PBR {
            vertex {
                return vec4(0.0)
            }
            fragment {
                in baseColor : color
                let result = baseColor.rgb
                [variant useEmission]
                let emission = vec3(1.0, 0.0, 0.0)
                return vec4(result, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    // The opt-in shape: #ifndef / #else / #endif. Without --define the
    // emission code is skipped; with `--define BGFX_VARIANT_USE_EMISSION`
    // shaderc selects the #else branch.
    CHECK(files.fragment.find("#ifndef BGFX_VARIANT_USE_EMISSION") != std::string::npos);
    CHECK(files.fragment.find("#else") != std::string::npos);
    CHECK(files.fragment.find("#endif") != std::string::npos);
    // The Phoskia [variant name] itself is NOT emitted as text.
    CHECK(files.fragment.find("[variant") == std::string::npos);
    CHECK(files.fragment.find("useEmission") == std::string::npos);
    // Statements after [variant] are still present in the #else branch.
    // The let emission = ... line should appear inside the #else block
    // (between #else and #endif).
    auto elsePos = files.fragment.find("#else");
    auto endifPos = files.fragment.find("#endif");
    auto letPos = files.fragment.find("emission = vec3(1.0, 0.0, 0.0)");
    CHECK(letPos != std::string::npos);
    CHECK(elsePos < letPos);
    CHECK(letPos < endifPos);
}

TEST_CASE(variant_expands_to_ifndef_in_vertex) {
    const char* src = R"(
        material PBR {
            vertex {
                in pos : position
                [variant skinning]
                let skinned = pos
                return vec4(skinned, 1.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vertex.find("#ifndef BGFX_VARIANT_SKINNING") != std::string::npos);
    CHECK(files.vertex.find("#else") != std::string::npos);
    CHECK(files.vertex.find("#endif") != std::string::npos);
}

TEST_CASE(multiple_variants_each_get_their_own_ifndef) {
    const char* src = R"(
        material PBR {
            vertex {
                return vec4(0.0)
            }
            fragment {
                in baseColor : color
                let r = baseColor.rgb
                [variant useEmission]
                r = r + vec3(1.0)
                [variant useFresnel]
                r = r * vec3(0.5)
                return vec4(r, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("#ifndef BGFX_VARIANT_USE_EMISSION") != std::string::npos);
    CHECK(files.fragment.find("#ifndef BGFX_VARIANT_USE_FRESNEL") != std::string::npos);
    CHECK(files.fragment.find("#else") != std::string::npos);
    // Two distinct #ifndef blocks ?two #endif blocks at least.
    size_t endifCount = 0, pos = 0;
    while ((pos = files.fragment.find("#endif", pos)) != std::string::npos) {
        ++endifCount;
        ++pos;
    }
    CHECK(endifCount >= 2);
}

TEST_CASE(variant_macro_name_uppercases_and_replaces_special_chars) {
    // Indirect test: variant name with mixed case + digit should still
    // produce a valid BGFX_VARIANT_<NAME> macro.
    const char* src = R"(
        material M {
            vertex {
                return vec4(0.0)
            }
            fragment {
                [variant HDR2Pass]
                return vec4(1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("#ifndef BGFX_VARIANT_HDR2PASS") != std::string::npos);
}

// ===== Phase 2 closing: PBR builtin inlining =====
//
// The four PBR math functions (fresnelSchlick / fresnelSchlickRoughness /
// distributionGGX / geometrySchlickGGX / geometrySmith) are registered
// as builtins (Step 3) for type-checking, but bgfx's shaderc does not
// know them. The BGFX converter inlines each call to the equivalent
// plain GLSL math at emission time. These tests pin the inlining
// shape so a refactor that subtly changes an emitted formula is
// caught.

TEST_CASE(pbr_fresnel_schlick_inlined_in_fs) {
    // After inlining, fresnelSchlick(cosTheta, F0) must produce a
    // GLSL expression matching F0 + (1 - F0) * pow(1 - cosTheta, 5),
    // NOT a call to fresnelSchlick (which shaderc would reject).
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let c = 0.5
                let f0 = vec3(0.04)
                let F = fresnelSchlick(c, f0)
                return vec4(F, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("fresnelSchlick") == std::string::npos);
    // pow(1.0 - 0.5, vec3(5.0)) inline expansion
    CHECK(files.fragment.find("pow(") != std::string::npos);
}

TEST_CASE(pbr_distribution_ggx_inlined_in_fs) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let NdotH = 0.9
                let r = 0.3
                let D = distributionGGX(NdotH, r)
                return vec4(D, D, D, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("distributionGGX") == std::string::npos);
    CHECK(files.fragment.find("3.14159265") != std::string::npos);
}

TEST_CASE(pbr_geometry_schlick_ggx_inlined_in_fs) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let NdotV = 0.8
                let r = 0.5
                let G = geometrySchlickGGX(NdotV, r)
                return vec4(G, G, G, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("geometrySchlickGGX") == std::string::npos);
}

TEST_CASE(pbr_geometry_smith_inlined_in_fs) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let NdotV = 0.7
                let NdotL = 0.6
                let r = 0.4
                let G = geometrySmith(NdotV, NdotL, r)
                return vec4(G, G, G, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("geometrySmith") == std::string::npos);
}

// ===== Scalar?vector broadcasting emission =====
//
// GLSL accepts component-wise arithmetic in either operand order
// (`vec3 * float` and `float * vec3` both yield vec3). The BGFX
// converter just emits the AST as a left-associative chain of
// parenthesized BinaryExprs, so the GLSL text preserves the user's
// operand order. These tests pin the emission shape so a refactor
// that reorders operands (e.g. always normalizing to `vec OP scalar`)
// is caught.
TEST_CASE(scalar_times_vec3_emits_scalar_first) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let s = 2.0
                let v = vec3(1.0, 2.0, 3.0)
                let r = s * v
                return vec4(r, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    // The let initializer is the user's expression verbatim. We just
    // want to see (s * v) or ((s) * (v)) in the output ?the
    // ordering of operands within the `*` should match the source.
    CHECK(files.fragment.find("r = (s * v);") != std::string::npos);
}

TEST_CASE(vec3_times_scalar_emits_vec3_first) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let s = 2.0
                let v = vec3(1.0, 2.0, 3.0)
                let r = v * s
                return vec4(r, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("r = (v * s);") != std::string::npos);
}

TEST_CASE(vec3_plus_scalar_emits_in_source_order) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                let s = 1.0
                let v = vec3(2.0, 3.0, 4.0)
                let r = v + s
                return vec4(r, 1.0)
            }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.fragment.find("r = (v + s);") != std::string::npos);
}

// ===== Phase 3.2: compute declaration ?BGFX .sc =====
//
// Earlier (Phase 2.5) the converter refused compute with a "HLSL /
// WGSL required" error. That conclusion was wrong ?bgfx 1.18 +
// shaderc 1.18 fully support compute via:
//   - shaderc --type compute
//   - bgfx::createProgram(ShaderHandle _csh)
//   - bgfx::dispatch(_handle, numGroupsX, numGroupsY, numGroupsZ)
//
// The new contract is:
//   - `result.success == true`
//   - `result.computeStages.size() == 1` per compute declaration
//   - `computeFiles[0].cs` is a valid bgfx .sc source with:
//       * `$input` / `$output` (both empty)
//       * `#include "common.sh"`
//       * `layout(local_size_x = 64) in;`
//       * `void main() { ... }` containing the lowered body
//   - Materials and computes in the same program both convert cleanly
//     (compute is no longer an error path)
//
// These tests pin the new contract so a future refactor that reverts
// to the error-stub behavior fails loudly.

TEST_CASE(compute_declaration_produces_valid_bgfx_cs) {
    const char* src = R"(
        compute ParticleUpdate { return 0 }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ir::IRGenerator gen;
    AYBGFXConverter conv;
    BGFXConvertResult res;
    conv.convertBGFX(gen.generate(*ast), res);
    CHECK(res.success);
    CHECK(res.errors.empty());
    CHECK(res.computeStages.size() == 1);
    const auto& cs = res.computeStages[0].compute;
    // Standard bgfx .sc prologue: $input / $output empty, common.sh include.
    CHECK(cs.find("$input") != std::string::npos);
    CHECK(cs.find("$output") != std::string::npos);
    CHECK(cs.find("#include \"common.sh\"") != std::string::npos);
    // Workgroup layout ?Phase 3.3 Block 2 default is (64, 1, 1) when
    // no `[numthreads(...)]` attribute is present. Phase 3.3 also
    // writes all three layout dimensions explicitly (GLSL would
    // default y and z to 1 anyway). Pin the literal so a refactor that
    // changes the default is caught (a real change needs an explicit
    // test update).
    CHECK(cs.find("layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;") != std::string::npos);
    // Body entry.
    CHECK(cs.find("void main()") != std::string::npos);
}

TEST_CASE(compute_alongside_material_converts_both) {
    // Non-fatal, both convert cleanly. Compute no longer reports an
    // error and the material still produces a three-piece set.
    const char* src = R"(
        material Unlit {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
        compute ParticleUpdate { return 0 }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ir::IRGenerator gen;
    AYBGFXConverter conv;
    BGFXConvertResult res;
    conv.convertBGFX(gen.generate(*ast), res);
    CHECK(res.success);
    CHECK(res.materialStages.size() == 1);
    CHECK(!res.materialStages[0].vertex.empty());
    CHECK(!res.materialStages[0].fragment.empty());
    CHECK(res.computeStages.size() == 1);
    CHECK(!res.computeStages[0].compute.empty());
}

// ===== Phase 1 RD-03: bone semantics + UBO array + skinningMatrix =====

TEST_CASE(uniform_block_array_field_emits_glsl_array_decl) {
    // Phase 1 RD-03: `mat4 bones[128]` UBO field must reach the GLSL
    // emit as `mat4 bones[128];` inside the `layout(std140, binding=...)
    // uniform Skeleton { ... } Skeleton;` block. Both the vs and fs
    // output splice the same _uboDecls string in (per the comment at
    // AYBGFXConverter.cpp:1474-1543), so we check one to verify the
    // emit path. The binding number is auto-assigned (0 in this case
    // since it's the only block).
    const char* src = R"(
        uniformblock Skeleton {
            mat4 bones[128]
        }
        material P {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.vertex.find("uniform Skeleton") != std::string::npos);
    CHECK(files.vertex.find("mat4 bones[128];") != std::string::npos);
    CHECK(files.fragment.find("uniform Skeleton") != std::string::npos);
    CHECK(files.fragment.find("mat4 bones[128];") != std::string::npos);
}

TEST_CASE(bone_semantics_emit_blendindices_blendweight_attrs) {
    // Phase 1 RD-03: a material with `in x : boneindices` and
    // `in y : boneweights` must produce a varying_def.sc that contains
    // the bgfx shaderc attribute names BLENDINDICES and BLENDWEIGHT
    // (these are the attribute slot names shaderc accepts; the
    // a_indices / a_weight names come from the BGFX semantic table).
    const char* src = R"(
        material P {
            vertex {
                in boneId : boneindices
                in boneWt : boneweights
                return vec4(boneId + boneWt)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto files = compileFirstMaterial(src);
    CHECK(files.varyingDefinitions.find("BLENDINDICES") != std::string::npos);
    CHECK(files.varyingDefinitions.find("BLENDWEIGHT") != std::string::npos);
    // And the $input line in the vertex stage must list both attrs.
    CHECK(files.vertex.find("a_indices") != std::string::npos);
    CHECK(files.vertex.find("a_weight") != std::string::npos);
}

TEST_CASE(skinning_matrix_call_expands_to_weighted_sum) {
    // Phase 1 RD-03: `skinningMatrix(indices, weights, bones, pos)` is
    // a builtin that must inline-expand to a 4-term linear-blend sum:
    //   w.x * bones[i.x] * pos + w.y * bones[i.y] * pos + ...
    // The BGFX backend renames the in-params to the bgfx attribute
    // names (boneindices → a_indices, boneweights → a_weight) before
    // emit, so the expansion references a_indices.x..w and a_weight.x..w.
    const char* src = R"(
        uniformblock Skeleton {
            mat4 bones[128]
        }
        material P {
            vertex {
                in boneId : boneindices
                in boneWt : boneweights
                let pos = vec4(1.0)
                return skinningMatrix(boneId, boneWt, Skeleton.bones, pos)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto files = compileFirstMaterial(src);
    // The expansion must reference the bones arg with index casts.
    CHECK(files.vertex.find("Skeleton.bones[int(a_indices.x)]") != std::string::npos);
    CHECK(files.vertex.find("Skeleton.bones[int(a_indices.y)]") != std::string::npos);
    CHECK(files.vertex.find("Skeleton.bones[int(a_indices.z)]") != std::string::npos);
    CHECK(files.vertex.find("Skeleton.bones[int(a_indices.w)]") != std::string::npos);
    // All four weight components must multiply their terms.
    CHECK(files.vertex.find("a_weight.x") != std::string::npos);
    CHECK(files.vertex.find("a_weight.y") != std::string::npos);
    CHECK(files.vertex.find("a_weight.z") != std::string::npos);
    CHECK(files.vertex.find("a_weight.w") != std::string::npos);
}

TEST_SUITE_END
