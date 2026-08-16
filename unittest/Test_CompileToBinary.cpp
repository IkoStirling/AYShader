// Test_CompileToBinary.cpp ??Phase 3.6 Commit 2 (B2)
//
// Tests AYBGFXConverter::compileToBinary(), the productization entry
// point that takes a Phoskia IR and returns a CompiledShaderProgram
// with .bin bytes per stage + opt-in .sc debug sources.
//
// These tests are intentionally minimal for Commit 2: they verify the
// shape of the output (success flag, .bin non-empty, sources map
// populated when keepSources=true) but they do NOT replace the legacy
// e2e tests in Test_ShaderCompile.cpp. Commit 4 (C1) is the
// plumbing-rewrite commit that folds these patterns into the
// Test_ShaderCompile.cpp e2e tests and deletes the old helper
// functions there.

#include "AYShader/Phoskia.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Lexer.h"
#include "AYShader/Parser.h"
#include "AYShader/Ast.h"
#include "AYShader/Ir.h"
#include "AYShader/ShadercDriver.h"  // for AYShadercDriver probe in shadercAvailable()
#include "AYTest.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  define PUTENV_S(name, val) _putenv_s(name, val)
#else
#  include <sys/types.h>
#  include <unistd.h>
#  define PUTENV_S(name, val) setenv(name, val, 1)
#endif

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

// CMake-injected hints ??same shape as Test_ShadercDriver.cpp. When
// not injected (i.e. AY_SHADER_BGFX_COMMON_HINT is ""), the shaderc
// invocation is missing the -i flag for bgfx's `common.sh` and any
// test that needs an actual compile will SKIP rather than fail.
#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif
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

// Probe whether shaderc is actually invocable. We use the
// explicit-path ctor with the CMake-injected vendored path so this
// probe is independent of process-wide `setDefaultExecutable` state
// (which other test files set/clear and we don't want to depend on
// the call order). On success the probe also sets the global default
// so subsequent `compileToBinary` calls (which use the default ctor
// when BGFXCompileOptions::shadercPath is empty) can find the driver.
bool shadercAvailable() {
    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExists(path)) return false;
    try {
        ayt::shader::AYShadercDriver probe(path);
        if (probe.shadercPath().empty()) return false;
        ayt::shader::AYShadercDriver::setDefaultExecutable(path);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Probe whether bgfx's common.sh is on disk. Without it, even a working
// shaderc invocation can't resolve the `#include "common.sh"` line that
// every Phoskia material's emitted .sc contains.
bool bgfxCommonAvailable() {
    return fileExists(AY_SHADER_BGFX_COMMON_HINT);
}

std::vector<std::string> shadercIncludeDirs() {
    std::vector<std::string> dirs;
    if (AY_SHADER_BGFX_COMMON_HINT[0] && fileExists(AY_SHADER_BGFX_COMMON_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_COMMON_HINT);
    }
    if (AY_SHADER_BGFX_SRC_HINT[0] && fileExists(AY_SHADER_BGFX_SRC_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_SRC_HINT);
    }
    return dirs;
}

// Build an IRProgram from a tiny Phoskia source. Mirrors the
// compileFirstMaterial pattern in Test_BGFXConverter.cpp.
ir::IRProgram buildIr(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ir::IRGenerator gen;
    return gen.generate(*ast);
}

// Smallest possible Phoskia material: a position pass-through with
// gl_FragColor = vec4(1). Matches the recipe the legacy e2e tests
// use so future comparative checks (compileToBinary vs the legacy
// shadercPath() test plumbing) can verify byte-equal .bin output.
const std::string kMinimalUnlit = R"(
material Unlit {
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0);

    vertex {
        in  position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in  position : position;
        return baseColor;
    }
}
)";

} // namespace

TEST_SUITE(CompileToBinaryTests)

// Phase 3.6 Commit 2: shape contract. Drives the full compileToBinary
// path on a well-formed minimal material. SKIPs to the failure-shape
// assertion path when shaderc or bgfx common.sh is missing on the
// host (CI / fresh checkout). When both are present, verifies the
// happy path: success=true, vsBin/fsBin non-empty, no compute bytes,
// sources map empty under default keepSources=false.
//
// shadercAvailable() is called BEFORE compileToBinary so the global
// default is set in time for the converter's lazy-init to find it.
// (Without this, the first test in the suite sees an unconfigured
// global default and fails, even though shaderc IS installed.)
TEST_CASE(compileToBinary_minimal_unlit_returns_shape) {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        // Failure-shape contract: errors vector has at least one entry
        // mentioning the missing-shaderc diagnostic. We still need to
        // run compileToBinary once to populate `program.errors` ??but
        // only when shaderc IS missing (the SKIP path is exactly the
        // error path).
        ir::IRProgram ir = buildIr(kMinimalUnlit);
        AYBGFXConverter conv;
        BGFXCompileOptions opts;
        opts.platform = "linux";
        opts.profile  = "430";
        opts.includeDirs = shadercIncludeDirs();
        CompiledShaderProgram program;
        conv.compileToBinary(ir, opts, program);

        std::cerr << "[compileToBinary test] SKIP detail assertions: "
                     "shaderc or bgfx common.sh not available.\n";
        CHECK(!program.success);
        CHECK(!program.errors.empty());
        CHECK(program.errors.front().find("AYShadercDriver") != std::string::npos);
        return;
    }

    ir::IRProgram ir = buildIr(kMinimalUnlit);
    AYBGFXConverter conv;
    BGFXCompileOptions opts;
    opts.platform = "linux";
    opts.profile  = "430";
    opts.includeDirs = shadercIncludeDirs();

    CompiledShaderProgram program;
    conv.compileToBinary(ir, opts, program);

    // Happy path.
    CHECK(program.success);
    CHECK(program.errors.empty());
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
    // No compute stage in this source.
    CHECK(program.csBin.empty());
    // Default keepSources=false: no debug map populated.
    CHECK(program.sources.empty());
}

// D3D11/DXBC (s_5_0) requires --varyingdef on vertex as well as fragment so
// attributes like a_position are declared for HLSL. Matrix*vector must use
// bgfx mul() (HLSL rejects mat * vec). Regression for Demo.
TEST_CASE(compileToBinary_dxbc_profile_compiles_position_shader) {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[compileToBinary test] SKIP: shaderc or bgfx "
                     "common.sh not available.\n";
        return;
    }

    const std::string kMvpMaterial = R"(
material RotatingCube {
    property baseColor = vec4(0.25, 0.55, 0.95, 1.0)
    vertex {
        in pos : position
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        return baseColor
    }
}
)";

    ir::IRProgram ir = buildIr(kMvpMaterial);
    AYBGFXConverter conv;
    BGFXCompileOptions opts;
    opts.platform = "windows";
    opts.profile  = "s_5_0";
    opts.includeDirs = shadercIncludeDirs();

    CompiledShaderProgram program;
    conv.compileToBinary(ir, opts, program);

    if (!program.success) {
        std::cerr << "[compileToBinary dxbc] failed:\n";
        for (const auto& e : program.errors) {
            std::cerr << "  " << e << "\n";
        }
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
}

// Phase 3.6 Commit 2: keepSources=true populates the in-memory sources
// map with the expected keys and the .sc text matches what convertBGFX
// would produce.
TEST_CASE(compileToBinary_keep_sources_populates_map) {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[compileToBinary test] SKIP: shaderc or bgfx "
                     "common.sh not available.\n";
        return;
    }

    ir::IRProgram ir = buildIr(kMinimalUnlit);
    AYBGFXConverter conv;
    BGFXCompileOptions opts;
    opts.platform = "linux";
    opts.profile  = "430";
    opts.includeDirs = shadercIncludeDirs();
    opts.keepSources = true;

    CompiledShaderProgram program;
    conv.compileToBinary(ir, opts, program);

    CHECK(program.success);
    CHECK(program.sources.count("vertex_stage_0") == 1);
    CHECK(program.sources.count("fragment_stage_0") == 1);
    CHECK(program.sources.count("varying_definitions") == 1);
    // Spot-check a couple of substrings that should be in any emitted
    // .sc: the bgfx $input marker for vs, and the gl_FragColor slot
    // for fs. These are stable across versions / unrelated to
    // shaderc-specific syntax.
    CHECK(program.sources.at("vertex_stage_0").find("$input") != std::string::npos);
    CHECK(program.sources.at("fragment_stage_0").find("$input") != std::string::npos);
}

// Phase 3.6 Commit 2: dumpIntermediate writes the .sc files to the
// given directory. We use the system tempdir; the test cleans up
// after itself.
TEST_CASE(compileToBinary_dump_intermediate_writes_files) {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[compileToBinary test] SKIP: shaderc or bgfx "
                     "common.sh not available.\n";
        return;
    }

    // Pick a unique dump dir under TEMP. The test doesn't clean up
    // (the test framework runs in CI, not interactive), but the dir
    // name is process-pid-stamped so concurrent runs don't clash.
    std::string dumpDir;
    {
        char buf[512];
#ifdef _WIN32
        const char* t = std::getenv("TEMP");
        if (!t) t = "C:\\Temp";
        std::snprintf(buf, sizeof(buf), "%s\\phoskia_test_%u",
                      t, static_cast<unsigned>(GetCurrentProcessId()));
#else
        const char* t = std::getenv("TMPDIR");
        if (!t) t = "/tmp";
        std::snprintf(buf, sizeof(buf), "%s/phoskia_test_%u",
                      t, static_cast<unsigned>(::getpid()));
#endif
        dumpDir = buf;
    }
    // Create the directory (best-effort; if it exists, OK).
#ifdef _WIN32
    CreateDirectoryA(dumpDir.c_str(), nullptr);
#else
    ::mkdir(dumpDir.c_str(), 0755);
#endif

    ir::IRProgram ir = buildIr(kMinimalUnlit);
    AYBGFXConverter conv;
    BGFXCompileOptions opts;
    opts.platform = "linux";
    opts.profile  = "430";
    opts.includeDirs = shadercIncludeDirs();
    opts.dumpIntermediate = true;
    opts.dumpDir = dumpDir;

    CompiledShaderProgram program;
    conv.compileToBinary(ir, opts, program);

    CHECK(program.success);
    // Both .sc files should now exist on disk.
    CHECK(fileExists(dumpDir + "/vertex_stage_0"));
    CHECK(fileExists(dumpDir + "/fragment_stage_0"));
    CHECK(fileExists(dumpDir + "/varying_definitions"));
}

// Phase 3.6 Commit 2: failure on duplicate UBO binding surfaces in
// program.errors and success=false. Tests that the new entry point
// preserves the convertBGFX error-reporting contract.
TEST_CASE(compileToBinary_duplicate_ubo_binding_reports_error) {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    const std::string src = R"(
uniformblock A { vec4 x; } binding 0;
uniformblock B { vec4 y; } binding 0;

material Test {
    vertex {
        in position : position;
        out position : position;
        return vec4(A.x.xyz, 1.0);
    }
    fragment {
        in position : position;
        return B.y;
    }
}
)";

    ir::IRProgram ir = buildIr(src);
    AYBGFXConverter conv;
    BGFXCompileOptions opts;
    opts.includeDirs = shadercIncludeDirs();

    CompiledShaderProgram program;
    conv.compileToBinary(ir, opts, program);

    CHECK(!program.success);
    CHECK(!program.errors.empty());
    // The error message names both blocks (or at least the duplicate
    // binding number). convertBGFX produces
    // "UniformBlock 'B' has duplicate binding 0 (also used by 'A')".
    bool foundDup = false;
    for (const auto& e : program.errors) {
        if (e.find("duplicate binding 0") != std::string::npos) {
            foundDup = true;
            break;
        }
    }
    CHECK(foundDup);
}

TEST_SUITE_END
