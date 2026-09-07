// ============================================================
// AYShader end-to-end Shader Compilation Test
// ============================================================
//
// Runs the full pipeline end-to-end through the Phase 3.6
// productization entry point:
//   Phoskia source -> Compiler::compileToProgram(src)
//                   -> CompiledShaderProgram::{vsBin, fsBin, csBin}
//
// This file used to own its own shaderc.exe path discovery, temp-dir
// staging, and CreateProcessW / popen invocation (lines 46-285 in the
// pre-Commit-4 file). Commit 4 retires all of that �?the plumbing
// now lives in `AYShadercDriver` and is driven by
// `Compiler::compileToProgram`.
//
// Each test still needs shaderc to actually be installed somewhere
// reachable (env AY_SHADER_SHADERC, CMake hint, or PATH). When it
// is not, the test fails with a clear FATAL diagnostic �?the Phase
// 1 e2e suite was mandatory, not "skip-if-no-shaderc". The harness
// on every developer machine has shaderc reachable; the diagnostic
// is there to surface it when something breaks the build setup.
//
// If shaderc fails to compile a Phoskia input, the test reports
// `out.errors` and `out.warnings` verbatim (this is much clearer
// than fishing the error out of a temp .bin file path).

#include "AYShader/Phoskia.h"
#include "AYShader/ShadercDriver.h"
#include "AYTest.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

// vcpkg shaderc path injected at CMake configure time. Same
// convention Test_ShadercDriver.cpp / Test_CompileToBinary.cpp use.
// (sign-off 2026-07-01: shaderc must be configured explicitly; we
// point the explicit-path ctor at the vendored binary. The test
// fails fast when the file isn't there, with a clear diagnostic.)
#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

// bgfx include paths for shaderc �?same trick Test_ShaderCompile.cpp
// used pre-Phase-3.6; empty when bgfx source wasn't located at
// configure time.
#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
#endif
#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT ""
#endif

inline bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// Shared shaderc-presence probe. We use the explicit-path ctor
// with the CMake-injected vendored path so the test is independent
// of the process-wide `setDefaultExecutable` state set by other
// test files. The diag string captures the driver's exception
// message so a missing binary produces an actionable diagnostic
// (not "not found").
//
// On success the probe also calls `setDefaultExecutable(path)` so
// subsequent `Compiler::compileToProgram` calls in the test
// resolve the driver via the global default (mirroring how a real
// engine uses it: configure once at startup, then call compileToProgram
// without per-call shaderc plumbing).
bool shadercReachable(std::string& diagOut) {
    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExists(path)) {
        diagOut = "configured shaderc not found at '" + path + "'";
        return false;
    }
    try {
        AYShadercDriver probe(path);
        if (probe.shadercPath().empty()) {
            diagOut = "driver ctor succeeded but path is empty";
            return false;
        }
        // Lock the global default so the test's compileToProgram
        // calls find the driver.
        AYShadercDriver::setDefaultExecutable(path);
        return true;
    } catch (const std::exception& e) {
        diagOut = e.what();
        return false;
    }
}

// Common FATAL preamble shared by every e2e test below. The `reason`
// string comes straight from the probe above �?it lists exactly
// which path was tried, so the user can tell whether the vendored
// binary is missing vs mis-configured.
void fatalNoShaderc(const char* testName, const std::string& reason) {
    std::cerr << "[" << testName << "] FATAL: shaderc not reachable. "
              << "Probe said: " << reason << "\n"
              << "Re-run CMake with -DAY_SHADER_SHADERC_PATH=/full/path/to/shaderc.exe "
              << "or vendor a shaderc binary at " << AY_SHADER_SHADERC_HINT << ".\n";
}

} // namespace

TEST_SUITE(ShaderCompileTests)

// ===== Minimal valid material: returns red vec4 from fragment =====

TEST_CASE(shaderc_compiles_minimal_unlit) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("minimal_unlit", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        material Unlit {
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    Compiler compiler;
    CompiledShaderProgram program = compiler.compileToProgram(src);
    if (!program.success) {
        std::cerr << "[minimal_unlit] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
    CHECK(program.csBin.empty());  // no compute stage
}

// ===== Compute end-to-end shaderc compile =====
//
// Phase 3.2 Block 4. Uses thread_id.x + storage buffer (Phase 3.2
// Blocks 2/3). Verify the byte buffer is non-empty (bgfx runtime
// rejects zero-byte programs at createProgram time).
TEST_CASE(shaderc_compiles_compute_with_storage_buffer) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("compute_storage", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        compute Increment {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )";
    Compiler compiler;
    CompiledShaderProgram program = compiler.compileToProgram(src);
    if (!program.success) {
        std::cerr << "[compute_storage] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.csBin.empty());
    CHECK(program.vsBin.empty());
    CHECK(program.fsBin.empty());
}

// ===== Material with texture sampling =====

TEST_CASE(shaderc_compiles_material_with_texture) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("with_texture", shadercDiag); CHECK(false); return; }

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
                return sample(albedoMap, uv)
            }
        }
    )";
    Compiler compiler;
    CompiledShaderProgram program = compiler.compileToProgram(src);
    if (!program.success) {
        std::cerr << "[with_texture] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
}

// ===== Full PBR material e2e =====
//
// A full Cook-Torrance PBR demo with all five PBR builtins (Fresnel-
// Schlick, Fresnel-Schlick-Roughness, GGX, Schlick-GGX, Smith), two
// texture samples (albedo + normal), and clearcoat + IBL. Validates
// the whole pipeline lowers to a working .bin pair.
//
// We also re-verify binding metadata through CompiledShaderProgram
// (textures + uniforms). This guards against future refactors that
// drop the PropertyDecl �?uniform registration path silently.
TEST_CASE(shaderc_compiles_pbr_with_ggx_and_fresnel) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("pbr", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        material PBR {
            texture2d albedoMap
            texture2d normalMap
            uniform mat4 modelViewProj
            uniform vec3 cameraPos
            uniform vec3 lightDir
            uniform float roughness
            uniform float metallic
            uniform float clearcoat
            uniform float clearcoatRoughness
            property emission = vec3(0.0, 0.0, 0.0)
            property envColor  = vec3(0.4, 0.45, 0.5)

            vertex {
                in pos    : position
                in nrm    : normal
                in uv     : texcoord
                out worldNormal : normal = vec3(0.0, 0.0, 1.0)
                out uvCoord     : texcoord = vec2(0.0, 0.0)
                return vec4(pos, 1.0)
            }

            fragment {
                in worldNormal : normal
                in uvCoord     : texcoord
                let N = normalize(worldNormal)
                let V = normalize(cameraPos)
                let L = normalize(lightDir)
                let H = normalize(L + V)
                let baseColor = sample(albedoMap, uvCoord)
                let _normalSample = sample(normalMap, uvCoord)
                let NdotV = max(dot(N, V), 0.001)
                let NdotL = max(dot(N, L), 0.0)
                let NdotH = max(dot(N, H), 0.0)
                let VdotH = max(dot(V, H), 0.0)
                let F0 = mix(vec3(0.04), baseColor.rgb, metallic)
                let F = fresnelSchlick(VdotH, F0)
                let Fcc = fresnelSchlickRoughness(VdotH, F0, clearcoatRoughness)
                let D = distributionGGX(NdotH, roughness)
                let G = geometrySmith(NdotV, NdotL, roughness)
                let Gcc = geometrySchlickGGX(NdotV, clearcoatRoughness)
                let specular = D * G * F / max(4.0 * NdotV, 0.001)
                let clearcoatSpec = D * Gcc * Fcc / max(4.0 * NdotV, 0.001) * clearcoat
                let diffuseIBL = baseColor.rgb * envColor * (vec3(1.0) - F) * (1.0 - metallic)
                let diffuse = baseColor.rgb * (vec3(1.0) - F) * (1.0 - metallic) / 3.14159265
                let result = diffuse + diffuseIBL + specular + clearcoatSpec
                [variant useEmission]
                result = result + emission
                return vec4(result, 1.0)
            }
        }
    )";

    Compiler compiler;
    CompiledShaderProgram program = compiler.compileToProgram(src);
    if (!program.success) {
        std::cerr << "[pbr] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());

    // Binding metadata must still be populated. (The PBR register
    // path is also covered by Test_BGFXConverter, but checking it
    // here keeps the e2e suite honest about its end-to-end claim.)
    bool hasAlbedo = false, hasNormal = false;
    for (const auto& t : program.textures) {
        if (t.name == "albedoMap") hasAlbedo = true;
        if (t.name == "normalMap") hasNormal = true;
    }
    CHECK(hasAlbedo);
    CHECK(hasNormal);

    int uniformCount = 0;
    bool hasEmission = false, hasEnvColor = false, hasRoughness = false;
    for (const auto& u : program.uniforms) {
        ++uniformCount;
        if (u.name == "emission")  hasEmission  = true;
        if (u.name == "envColor")  hasEnvColor  = true;
        if (u.name == "roughness") hasRoughness = true;
    }
    CHECK(uniformCount >= 9);  // 5 user uniforms + 2 properties + clearcoat(2)
    CHECK(hasEmission);
    CHECK(hasEnvColor);
    CHECK(hasRoughness);
}

// ===== UBO end-to-end via shaderc =====
//
// Phase 3.4 brought UBO support; this is the end-to-end proof that
// a material with a top-level uniformblock still compiles to a
// working GLSL 4.30 vs/fs pair (the .bin pair that bgfx::createProgram
// accepts). The UBO decl carries std140 layout + binding slot 0.
//
// For the .sc-text check (Phase 3.4 contract: both vs and fs include
// the UBO decl) we use keepSources so we can read vertex_stage_0 / fragment_stage_0
// out of the sources map. This was previously an `m.vs.find(...)` on
// BGFXShaderFiles; that field is still readable but `compileToProgram`
// is the new canonical path.
TEST_CASE(shaderc_compiles_material_with_ublock) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("ublock", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        uniformblock Camera {
            vec3 position
            float fov
        }
        material UBOTest {
            vertex {
                let p = Camera.position
                return vec4(p, 1.0)
            }
            fragment {
                return vec4(Camera.fov, 0.0, 0.0, 1.0)
            }
        }
    )";
    Compiler compiler;
    CompileOptions opts;
    opts.keepSources = true;  // for the layout() text check below
    CompiledShaderProgram program = compiler.compileToProgram(src, opts);
    if (!program.success) {
        std::cerr << "[ublock] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());

    // .sc text check: UBO decl appears in both vs and fs. The
    // binding is the default (0) for an unannotated UBO.
    CHECK(program.sources.count("vertex_stage_0") == 1);
    CHECK(program.sources.count("fragment_stage_0") == 1);
    CHECK(program.sources.at("vertex_stage_0").find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
    CHECK(program.sources.at("fragment_stage_0").find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
}

// ===== Phase 3.5-B: UBO explicit binding slot =====

TEST_CASE(shaderc_compiles_material_with_ublock_binding) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("ublock_bind", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        uniformblock Camera {
            vec3 position
            float fov
        } binding 7
        material UBOMat {
            vertex {
                let p = Camera.position
                return vec4(p, 1.0)
            }
            fragment {
                return vec4(Camera.fov, 0.0, 0.0, 1.0)
            }
        }
    )";
    Compiler compiler;
    CompileOptions opts;
    opts.keepSources = true;
    CompiledShaderProgram program = compiler.compileToProgram(src, opts);
    if (!program.success) {
        std::cerr << "[ublock_bind] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());

    // Phase 3.5-B: explicit binding 7 must reach GLSL verbatim.
    CHECK(program.sources.count("vertex_stage_0") == 1);
    CHECK(program.sources.count("fragment_stage_0") == 1);
    CHECK(program.sources.at("vertex_stage_0").find("layout(std140, binding = 7) uniform Camera {") != std::string::npos);
    CHECK(program.sources.at("fragment_stage_0").find("layout(std140, binding = 7) uniform Camera {") != std::string::npos);
}

// ===== Phase 3.5-A: storage decl explicit binding slot =====

TEST_CASE(shaderc_compiles_compute_with_storage_binding_to_bin) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("compute_storage_binding", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        compute Increment {
            storage counters : rwstructuredbuffer<int> binding 1
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )";
    Compiler compiler;
    CompileOptions opts;
    opts.keepSources = true;  // for the .sc layout() check
    CompiledShaderProgram program = compiler.compileToProgram(src, opts);
    if (!program.success) {
        std::cerr << "[compute_storage_binding] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.csBin.empty());

    // Binding metadata must reflect user binding 1.
    CHECK(program.storageBuffers.size() == 1);
    CHECK(program.storageBuffers[0].name == "counters");
    CHECK(program.storageBuffers[0].binding == 1);

    // .sc text check: explicit binding �?layout(std430, binding = 1).
    CHECK(program.sources.count("compute_stage_0") == 1);
    CHECK(program.sources.at("compute_stage_0").find("layout(std430, binding = 1) buffer counters {") != std::string::npos);
}

TEST_CASE(shaderc_compiles_compute_with_two_storage_buffers_to_bin) {
    std::string shadercDiag; if (!shadercReachable(shadercDiag)) { fatalNoShaderc("compute_two_storage", shadercDiag); CHECK(false); return; }

    const char* src = R"(
        compute Move {
            storage inputs  : structuredbuffer<float> binding 0
            storage outputs : rwstructuredbuffer<float> binding 1
            let idx = thread_id.x
            outputs[idx] = inputs[idx] + 1.0
        }
    )";
    Compiler compiler;
    CompileOptions opts;
    opts.keepSources = true;
    CompiledShaderProgram program = compiler.compileToProgram(src, opts);
    if (!program.success) {
        std::cerr << "[compute_two_storage] compileToProgram failed:\n";
        for (const auto& e : program.errors) std::cerr << "  err: " << e << "\n";
    }
    CHECK(program.success);
    CHECK(!program.csBin.empty());

    CHECK(program.storageBuffers.size() == 2);
    CHECK(program.storageBuffers[0].binding == 0);
    CHECK(program.storageBuffers[1].binding == 1);

    CHECK(program.sources.count("compute_stage_0") == 1);
    CHECK(program.sources.at("compute_stage_0").find("layout(std430, binding = 0) buffer inputs {") != std::string::npos);
    CHECK(program.sources.at("compute_stage_0").find("layout(std430, binding = 1) buffer outputs {") != std::string::npos);
}

TEST_CASE(shaderc_compiles_fragment_if_and_discard) {
    std::string shadercDiag;
    if (!shadercReachable(shadercDiag)) {
        fatalNoShaderc("fragment_control_flow", shadercDiag);
        CHECK(false);
        return;
    }
    const char* src = R"(
        material Cutout {
            vertex { return vec4(0.0) }
            fragment {
                let alpha = 0.25
                if (alpha < 0.1) { discard }
                let color = vec4(0.0)
                if (alpha < 0.5) { color = vec4(1.0) }
                return color
            }
        }
    )";
    Compiler compiler;
    CompiledShaderProgram program = compiler.compileToProgram(src);
    if (!program.success) {
        for (const auto& error : program.errors) {
            std::cerr << "[fragment_control_flow] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
}

TEST_SUITE_END
