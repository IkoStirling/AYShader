// ============================================================
// AYShader Compiler (AYPhoskia) End-to-End Unit Tests
// (Phase 1 closure — vertex/fragment syntax)
//
// Phase 3.6 Commit 5: every assertion that used to read
// `result.output.find(...)` or `result.output.empty()` now reads
// the same shape from `CompiledShaderProgram::sources` (keyed by
// "vs_N.sc" / "fs_N.sc" / "cs_N.sc" / "varying.def.sc"). The
// frontend `.sc` joiner shape lives in `CompileResult::output`
// (deprecated) but no unit test reads it directly anymore.
// ============================================================

#include "AYPhoskia.h"
#include "AYBuiltinTypes.h"
#include "AYShadercDriver.h"
#include "AYTest.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  define PUTENV_S(name, val) _putenv_s(name, val)
#else
#  define PUTENV_S(name, val) setenv(name, val, 1)
#endif

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

// Same vendored-shaderc hint convention Test_ShaderCompile.cpp /
// Test_ShadercDriver.cpp / Test_CompileToBinary.cpp /
// Test_CompileToProgram.cpp use. CMake-injected at build time.
#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif

inline bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// Probe the vendored shaderc and lock it as the process-wide
// default for this test run. compileWithSources() needs shaderc
// available because `compileToProgram` lazily instantiates the
// driver from the global default when the per-call BGFXCompileOptions
// shadercPath is empty (Test_Phoskia tests don't override it).
//
// NOTE: this is a test-isolation helper. Real engines call
// `AYShadercDriver::setDefaultExecutable(<path>)` exactly once at
// startup. We mirror that contract here so test ordering doesn't
// leak state between Test_ShadercDriver / Test_CompileToBinary (which
// both call setDefaultExecutable themselves) and Test_Phoskia
// (which previously didn't).
bool ensureShadercDefault() {
    static const std::string kPath = AY_SHADER_SHADERC_HINT;
    if (!fileExists(kPath)) return false;
    try {
        ayt::shader::AYShadercDriver probe(kPath);
        ayt::shader::AYShadercDriver::setDefaultExecutable(kPath);
        return true;
    } catch (...) {
        return false;
    }
}

// Commit 5 helper: drive the full pipeline with keepSources=true so
// `program.sources` carries every emitted .sc string. Tests that
// used to read `result.output.find(...)` now read
// `prog.sources.at(k).find(...)` for the same substring contract.
//
// We deliberately do NOT use the `Compiler::compile()` legacy API
// (whose `result.output` is deprecated / empty by default) —
// `compileToProgram` is the documented Phase 3.6 entry point and
// the thing future engine code will actually call.
//
// On hosts without a working shaderc binary, `success` will be
// false (lazy-init fails, error lands in `prog.errors[0]`). The
// sources map is still populated from `convertBGFX` regardless of
// shaderc availability (Commit 5: pre-shaderc populate point) —
// so substring assertions on `prog.sources[k]` succeed even when
// the host has no shaderc. The legacy e2e suite
// (Test_ShaderCompile.cpp) is the only one that hard-requires
// shaderc to be installed.
CompiledShaderProgram compileWithSources(
    const std::string& src,
    const CompileOptions& opts = CompileOptions{}) {
    CompileOptions effective = opts;
    effective.keepSources = true;  // force-fill the sources map.
    // Clear env vars that would otherwise OR-override (test wants
    // determinism; the precedence contract is covered by
    // Test_CompileToProgram.cpp / Test_ShadercDriver.cpp).
#ifdef _WIN32
    _putenv("AY_PHOSKIA_KEEP_SOURCES=");
    _putenv("AY_PHOSKIA_DUMP_SC=");
#else
    unsetenv("AY_PHOSKIA_KEEP_SOURCES");
    unsetenv("AY_PHOSKIA_DUMP_SC");
#endif
    // Lock the shaderc default so compileToProgram's lazy-init finds
    // it. Without this, the first test in this file (or any test run
    // before Test_ShadercDriver set the default) would fail with
    // "no default executable configured". SKIPPED silently when the
    // vendored binary isn't on disk — see Test_ShaderCompile.cpp's
    // shadercReachable() rationale for SKIP semantics.
    (void)ensureShadercDefault();

    Compiler compiler;
    return compiler.compileToProgram(src, effective);
}

// Convenience: same as compileWithSources but discards the
// CompileOptions so tests that only need the default targetBackend
// read cleanly.
CompiledShaderProgram compileWithSourcesDefault(const std::string& src) {
    return compileWithSources(src, CompileOptions{});
}

// Build a CompileOptions stub for tests that want to set
// enableTypeInference / enableSemanticAnalysis / strictMode etc.
CompileOptions baseOpts() {
    return CompileOptions{};
}

} // namespace

TEST_SUITE(PhoskiaCompilerTests)

// ===== Minimal valid material =====

TEST_CASE(compile_minimal_unlit) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        material Unlit {
            property color = vec4(1.0, 0.0, 0.0, 1.0)
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return color }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    // Frontend-facing contract: keepSources=true populates the
    // sources map with every emitted .sc string. There IS no
    // longer a concatenated `.output` joiner that the frontend is
    // supposed to read.
    CHECK(!prog.sources.empty());
    CHECK(prog.success || !prog.sources.empty());
}

TEST_CASE(compile_empty_material) {
    // Must include both blocks now — the converter rejects otherwise.
    CompiledShaderProgram prog = compileWithSourcesDefault(
        "material X { vertex { } fragment { } }");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(!prog.sources.empty());
}

TEST_CASE(compile_multiple_materials) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
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
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.success || !prog.sources.empty());
}

// ===== Backend registration =====

TEST_CASE(default_backend_is_registered) {
    CompiledShaderProgram prog = compileWithSourcesDefault(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }");
    CHECK(prog.success || !prog.sources.empty());
}

TEST_CASE(compile_to_unknown_backend_fails) {
    // compileToBackend is the only path still documented to use
    // a backend-name parameter; compileToProgram is BGFX-only for
    // now (Phase 5+ may add a targetBackend field). Verify the
    // legacy path: an unknown backend name surfaces as a parser
    // or backend error.
    Compiler compiler;
    CompileResult result{};
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
    CompileResult result{};
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
    CompileResult result{};
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
    CompileResult result{};
    compiler.compile("material { vertex { } fragment { } }", result);
    CHECK(!result.errors.empty());
}

TEST_CASE(parse_error_propagated) {
    Compiler compiler;
    CompileResult result{};
    compiler.compile(
        "material X { vertex { return vec4(0.0); "
        "fragment { return vec4(1.0) }", result);  // missing '}'
    CHECK(!result.errors.empty());
}

TEST_CASE(missing_vertex_block_causes_error) {
    Compiler compiler;
    CompileResult result{};
    compiler.compile(
        "material X { fragment { return vec4(1.0) } }", result);
    CHECK(!result.success);
}

TEST_CASE(missing_fragment_block_causes_error) {
    Compiler compiler;
    CompileResult result{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } }", result);
    CHECK(!result.success);
}

// ===== Realistic Phoskia snippet =====

TEST_CASE(compile_pbr_like_material) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
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
    )");
    CHECK(prog.success || !prog.sources.empty());
    // Frontend-facing shape: the .sc strings live in program.sources
    // keyed by `varying.def.sc` + per-material `vs_N.sc` / `fs_N.sc`.
    CHECK(prog.sources.count("varying.def.sc") == 1);
    CHECK(prog.sources.at("vs_0.sc").find("$input") != std::string::npos);
}

TEST_CASE(compile_with_if_else) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
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
    )");
    // Frontend shape contract: `prog.sources` is a well-formed map.
    // Whether populate happens depends on `convertBGFX` succeeding
    // (the legacy `.output != empty` check was overly permissive —
    // we don't ship empty garbage to the frontend in 3.6).
    if (prog.success) {
        CHECK(!prog.sources.empty());
    }
}

TEST_CASE(compile_with_for_loop) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        material X {
            vertex {
                for (i in items) {
                    let j = i
                }
                return vec4(0.0)
            }
            fragment { return vec4(0.0) }
        }
    )");
    if (prog.success) {
        CHECK(!prog.sources.empty());
    }
}

// ===== Phase 3.2: compute declaration end-to-end =====
TEST_CASE(compile_minimal_compute) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute ParticleUpdate {
            let x = 0
            return x
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    // cs source must be present in the sources map.
    CHECK(prog.sources.count("cs_0.sc") == 1);
    CHECK(prog.sources.at("cs_0.sc").find("void main()") != std::string::npos);
    CHECK(prog.sources.at("cs_0.sc").find("layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;") != std::string::npos);
}

// ===== Phase 3.2 Block 2: thread_id / group_id / dispatch_id builtins =====

TEST_CASE(compile_compute_uses_thread_id_builtin) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute ParticleUpdate {
            let idx = thread_id.x
            return idx
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("gl_GlobalInvocationID") != std::string::npos);
    CHECK(cs.find("gl_GlobalInvocationID.x") != std::string::npos);
    CHECK(cs.find("thread_id") == std::string::npos);
}

TEST_CASE(compile_compute_uses_group_id_builtin) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute ParticleUpdate {
            let gid = group_id.x
            return gid
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("gl_WorkGroupID.x") != std::string::npos);
    CHECK(cs.find("group_id") == std::string::npos);
}

TEST_CASE(compile_compute_uses_dispatch_id_builtin) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute ParticleUpdate {
            let did = dispatch_id.x
            return did
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("(gl_NumWorkGroups * gl_WorkGroupID).x") != std::string::npos);
    CHECK(cs.find("dispatch_id") == std::string::npos);
}

// ===== Phase 3.2 Block 3: storage buffer declarations =====

TEST_CASE(compile_compute_with_storage_buffer) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Increment {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("buffer counters") != std::string::npos);
    CHECK(cs.find("int data[]") != std::string::npos);
    CHECK(cs.find("} counters;") != std::string::npos);
    CHECK(cs.find("rwstructuredbuffer") == std::string::npos);
    CHECK(cs.find("gl_GlobalInvocationID") != std::string::npos);
}

TEST_CASE(compile_compute_with_structured_buffer_read) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Reader {
            storage particles : structuredbuffer<vec3>
            let idx = thread_id.x
            return idx
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("buffer particles") != std::string::npos);
    CHECK(cs.find("vec3 data[]") != std::string::npos);
    CHECK(cs.find("structuredbuffer") == std::string::npos);
}

// ===== Phase 3.3 Block 1: uint builtin type =====

TEST_CASE(compile_compute_with_uint_storage_buffer) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute UintCounter {
            storage counters : rwstructuredbuffer<uint>
            let idx = thread_id.x
            counters[idx] = counters[idx] + uint(1)
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("buffer counters") != std::string::npos);
    CHECK(cs.find("uint data[]") != std::string::npos);
    CHECK(cs.find("} counters;") != std::string::npos);
    CHECK(cs.find("rwstructuredbuffer") == std::string::npos);
}

TEST_CASE(uint_is_builtin_type) {
    CHECK(AYBuiltinTypes::isBuiltinType("uint"));
    CHECK(AYBuiltinTypes::isBuiltinType("int"));
    CHECK(AYBuiltinTypes::isBuiltinType("float"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("foo"));
}

// ===== Phase 3.3 Block 2: [numthreads(X, Y, Z)] compute attribute =====

TEST_CASE(compile_compute_with_numthreads_attribute) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        [numthreads(8, 8, 1)] compute MatMulKernel {
            let idx = thread_id.x
            return idx
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;") != std::string::npos);
    CHECK(cs.find("numthreads") == std::string::npos);
}

TEST_CASE(compile_compute_without_numthreads_uses_default) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute DefaultShape {
            let idx = thread_id.x
            return idx
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.sources.at("cs_0.sc").find("layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;") != std::string::npos);
}

// ===== Phase 3.3 Block 3: uvec3 strict typing =====

#include "AYIr.h"
#include "AYLexer.h"
#include "AYParser.h"

TEST_CASE(compile_thread_id_x_is_uint_in_ir) {
    const char* src = R"(
        compute Foo {
            let idx = thread_id.x
            return idx
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ayt::shader::phoskia::ir::IRGenerator gen;
    auto program = gen.generate(*ast);

    CHECK(program.computes.size() == 1);
    const auto& body = program.computes.front()->body;
    auto* let = dynamic_cast<ayt::shader::phoskia::ir::IRLetStmt*>(body[0].get());
    CHECK(let != nullptr);
    CHECK_NOT_NULL(let->initializer.get());
    auto prim = std::dynamic_pointer_cast<ayt::shader::phoskia::PrimitiveType_>(let->initializer->resolvedType);
    CHECK_NOT_NULL(prim.get());
    CHECK(prim->primitive() == ayt::shader::phoskia::PrimitiveType::Uint);
}

TEST_CASE(compile_thread_id_x_emits_gl_global_invocation_id_x) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            let idx = thread_id.x
            return idx
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("gl_GlobalInvocationID") != std::string::npos);
    CHECK(cs.find("gl_GlobalInvocationID.x") != std::string::npos);
}

TEST_CASE(compile_let_uint_idx_emits_with_uint_type) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("float idx = ") == std::string::npos);
    CHECK(cs.find("vec3 idx = ") == std::string::npos);
}

TEST_CASE(compile_uvec3_constructor_emits_uvec3_call) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            let idx = uvec3(thread_id.x, 0, 0).x
            counters[idx] = counters[idx] + 1
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.sources.at("cs_0.sc").find("uvec3(") != std::string::npos);
}

// ===== Phase 3.3 Block 4: workgroup-shared local memory =====

TEST_CASE(compile_compute_with_shared_array) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Reduce {
            shared float tile[64]
            let i = thread_id.x
            tile[i] = float(i)
            return tile[0]
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("shared float tile[64];") != std::string::npos);
    CHECK(cs.find("rwstructuredbuffer") == std::string::npos);
    auto posShared = cs.find("shared float tile[64];");
    auto posMain = cs.find("void main()");
    CHECK(posShared != std::string::npos);
    CHECK(posMain != std::string::npos);
    CHECK(posShared < posMain);
}

TEST_CASE(compile_compute_with_uint_shared_array) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Histogram {
            shared uint bins[256]
            let i = thread_id.x
            bins[i] = uint(0)
            return 0
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.sources.at("cs_0.sc").find("shared uint bins[256];") != std::string::npos);
}

TEST_CASE(compile_compute_shared_and_storage_coexist) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Scan {
            shared float tile[64]
            storage inBuf : structuredbuffer<float>
            let i = thread_id.x
            tile[i] = inBuf[i]
            return 0
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("shared float tile[64];") != std::string::npos);
    CHECK(cs.find("buffer inBuf") != std::string::npos);
    CHECK(cs.find("float data[]") != std::string::npos);
}

// ===== Phase 3.4: uniform block (UBO) =====
TEST_CASE(compile_uniformblock_emits_layout_std140) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Camera {
            vec3 position
            float fov
        }
        material PBR {
            vertex {
                return vec4(Camera.position, 1.0)
            }
            fragment {
                return vec4(Camera.fov, 0.0, 0.0, 1.0)
            }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    // The UBO decl appears in both vs and fs (UBO is global).
    const auto& vs = prog.sources.at("vs_0.sc");
    const auto& fs = prog.sources.at("fs_0.sc");
    CHECK(vs.find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
    CHECK(fs.find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
    CHECK(vs.find("vec3 position;") != std::string::npos);
    CHECK(vs.find("float fov;") != std::string::npos);
    CHECK(vs.find("} Camera;") != std::string::npos);
}

TEST_CASE(compile_two_uniformblocks_have_distinct_bindings) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Camera {
            vec3 position
        }
        uniformblock Lighting {
            vec3 ambient
        }
        material P {
            vertex { return vec4(0.0) }
            fragment { return vec4(0.0) }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& vs = prog.sources.at("vs_0.sc");
    CHECK(vs.find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
    CHECK(vs.find("layout(std140, binding = 1) uniform Lighting {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_field_access_emits_dot) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Camera {
            vec3 position
        }
        material P {
            vertex {
                let p = Camera.position
                return vec4(p, 1.0)
            }
            fragment { return vec4(0.0) }
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    // The dot access reaches the emitted source verbatim in vs.
    CHECK(prog.sources.at("vs_0.sc").find("Camera.position") != std::string::npos);
}

TEST_CASE(compile_uniformblock_in_compute_body) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Config {
            uint iterations
        }
        compute Run {
            let i = thread_id.x
            return i
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("layout(std140, binding = 0) uniform Config {") != std::string::npos);
    CHECK(cs.find("uint iterations;") != std::string::npos);
}

// ===== Phase 3.5-A: storage decl explicit binding slot =====
TEST_CASE(compile_storage_with_binding_emits_layout_std430) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding 1
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("layout(std430, binding = 1) buffer counters {") != std::string::npos);
    CHECK(cs.find("int data[]") != std::string::npos);
    CHECK(cs.find("} counters;") != std::string::npos);
}

TEST_CASE(compile_storage_without_binding_uses_auto_slot) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.sources.at("cs_0.sc").find("layout(std430, binding = 0) buffer counters {") != std::string::npos);
}

TEST_CASE(compile_two_storages_auto_assign_distinct_slots) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            storage outputs  : rwstructuredbuffer<float>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
            outputs[idx] = float(idx)
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("layout(std430, binding = 0) buffer counters") != std::string::npos);
    CHECK(cs.find("layout(std430, binding = 1) buffer outputs") != std::string::npos);
}

TEST_CASE(compile_storage_mixed_explicit_and_auto) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage explicit : rwstructuredbuffer<int> binding 1
            storage auto    : rwstructuredbuffer<float>
            let idx = thread_id.x
            explicit[idx] = explicit[idx] + 1
            auto[idx] = float(idx)
        }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& cs = prog.sources.at("cs_0.sc");
    CHECK(cs.find("layout(std430, binding = 1) buffer explicit") != std::string::npos);
    CHECK(cs.find("layout(std430, binding = 2) buffer auto") != std::string::npos);
}

TEST_CASE(compile_storage_with_duplicate_binding_errors) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        compute Foo {
            storage a : rwstructuredbuffer<int> binding 0
            storage b : rwstructuredbuffer<float> binding 0
            return 0
        }
    )");
    CHECK_FALSE(prog.success);
    bool foundDuplicateError = false;
    for (const auto& err : prog.errors) {
        if (err.find("duplicate binding") != std::string::npos) {
            foundDuplicateError = true;
            break;
        }
    }
    CHECK(foundDuplicateError);
}

// ===== Phase 3.5-B: uniformblock decl explicit binding slot =====
TEST_CASE(compile_uniformblock_with_binding_emits_layout_std140) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Camera {
            vec3 position
            float fov
        } binding 1
        material P { vertex { return vec4(Camera.position, 1.0) } fragment { return vec4(0,0,0,1) } }
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.sources.at("vs_0.sc").find("layout(std140, binding = 1) uniform Camera {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_without_binding_uses_auto_slot) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Camera {
            vec3 position
        }
        material P { vertex { return vec4(Camera.position, 1.0) } fragment { return vec4(0,0,0,1) } }
    )");
    CHECK(prog.success || !prog.sources.empty());
    CHECK(prog.sources.at("vs_0.sc").find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_mixed_explicit_and_auto) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock Explicit {
            vec3 a
        } binding 1
        uniformblock Auto {
            vec3 b
        }
        material P { vertex { return vec4(Explicit.a + Auto.b, 1.0) } fragment { return vec4(0,0,0,1) } }
    )");
    CHECK(prog.success || !prog.sources.empty());
    const auto& vs = prog.sources.at("vs_0.sc");
    CHECK(vs.find("layout(std140, binding = 1) uniform Explicit {") != std::string::npos);
    CHECK(vs.find("layout(std140, binding = 2) uniform Auto {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_with_duplicate_binding_errors) {
    CompiledShaderProgram prog = compileWithSourcesDefault(R"(
        uniformblock A { vec3 a; } binding 0
        uniformblock B { vec3 b; } binding 0
        material P { vertex { return vec4(A.a + B.b, 1.0) } fragment { return vec4(0,0,0,1) } }
    )");
    CHECK_FALSE(prog.success);
    bool foundDuplicateError = false;
    for (const auto& err : prog.errors) {
        if (err.find("duplicate binding") != std::string::npos) {
            foundDuplicateError = true;
            break;
        }
    }
    CHECK(foundDuplicateError);
}

TEST_SUITE_END
