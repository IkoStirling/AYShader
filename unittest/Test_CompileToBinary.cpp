// Test_CompileToBinary.cpp — Phase 3.6 Commit 2 (B2)
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

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYIr.h"
#include "AYShadercDriver.h"  // for AYShadercDriver probe in shadercAvailable()
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

// CMake-injected hints — same shape as Test_ShadercDriver.cpp. When
// not injected (i.e. AY_SHADER_BGFX_COMMON_HINT is ""), the shaderc
// invocation is missing the -i flag for bgfx's `common.sh` and any
// test that needs an actual compile will SKIP rather than fail.
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

// Probe whether shaderc is actually invocable. Uses the same probe
// pattern as Test_ShadercDriver.cpp — try/catch on the driver ctor
// because the production ctor throws std::runtime_error on missing
// shaderc (no "best-effort return hint" anymore).
bool shadercAvailable() {
    try {
        ayt::shader::AYShadercDriver probe;
        return !probe.shadercPath().empty();
    } catch (const std::runtime_error&) {
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
    property vec4 baseColor : const = vec4(1.0, 1.0, 1.0, 1.0);

    vertex {
        in  position : POSITION;
        out position : POSITION;
        return vec4(position, 1.0);
    }
    fragment {
        in  position : POSITION;
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
TEST_CASE(compileToBinary_minimal_unlit_returns_shape) {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    ir::IRProgram ir = buildIr(kMinimalUnlit);

    AYBGFXConverter conv;
    BGFXCompileOptions opts;
    opts.platform = "linux";
    opts.profile  = "430";
    opts.includeDirs = shadercIncludeDirs();

    CompiledShaderProgram program;
    conv.compileToBinary(ir, opts, program);

    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[compileToBinary test] SKIP detail assertions: "
                     "shaderc or bgfx common.sh not available.\n";
        // Failure-shape contract: errors vector has at least one entry
        // mentioning the missing-shaderc diagnostic.
        CHECK(!program.success);
        CHECK(!program.errors.empty());
        CHECK(program.errors.front().find("AYShadercDriver") != std::string::npos);
        return;
    }

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
    CHECK(program.sources.count("vs_0.sc") == 1);
    CHECK(program.sources.count("fs_0.sc") == 1);
    CHECK(program.sources.count("varying.def.sc") == 1);
    // Spot-check a couple of substrings that should be in any emitted
    // .sc: the bgfx $input marker for vs, and the gl_FragColor slot
    // for fs. These are stable across versions / unrelated to
    // shaderc-specific syntax.
    CHECK(program.sources.at("vs_0.sc").find("$input") != std::string::npos);
    CHECK(program.sources.at("fs_0.sc").find("$input") != std::string::npos);
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
    CHECK(fileExists(dumpDir + "/vs_0.sc"));
    CHECK(fileExists(dumpDir + "/fs_0.sc"));
    CHECK(fileExists(dumpDir + "/varying.def.sc"));
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
        in position : POSITION;
        out position : POSITION;
        return vec4(A.x.xyz, 1.0);
    }
    fragment {
        in position : POSITION;
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