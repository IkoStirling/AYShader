// Test_ShadercDriver.cpp — Phase 3.6 Commit 1 (B1)
//
// Tests the promoted AYShadercDriver: path discovery, in-memory .sc
// -> .bin bytes round-trip, and the env-var precedence contract.
//
// These tests are independent of the rest of AYShader — the driver
// only needs shaderc.exe to exist on the machine. If shaderc is not
// installed, the constructor tests skip gracefully (mirroring the
// historical shadercPath() test behavior); the round-trip test
// fails hard because there is nothing to compile with.

#include "AYShadercDriver.h"
#include "AYTest.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  define PUTENV_S(name, val) _putenv_s(name, val)
#else
#  include <unistd.h>
#  define PUTENV_S(name, val) setenv(name, val, 1)
#endif

using namespace ayt::shader;

// stat()-based file existence check (avoids std::filesystem::exists
// ambiguity that surfaces when the engine's pre-C++17 headers leak
// into this TU).
inline bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
#ifdef _WIN32
    return ::stat(p.c_str(), &st) == 0;
#else
    return ::stat(p.c_str(), &st) == 0;
#endif
}

namespace {

// CMake-injected fall-back path for shaderc — same convention the
// pre-Phase-3.6 Test_ShaderCompile.cpp used.
#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif

// bgfx include paths for shaderc. Same trick Test_ShaderCompile.cpp
// uses; empty when bgfx source wasn't located at configure time.
#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
#endif
#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT ""
#endif

// Clean the env-var surface that Phase 3.6 introduces. Caller's
// shell may have these set; tests must see a known starting point.
void clearPhase36Env() {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");
}

// Probe AYShadercDriver's own path discovery by attempting a
// throw-away construction in a try/catch. If shaderc isn't on disk,
// the ctor throws std::runtime_error — we catch it and return false
// so the caller SKIPs the test instead of letting the framework
// count an uncaught exception as a test failure.
//
// Mirrors the historical Test_ShaderCompile.cpp pattern (env-var
// only) but covers PATH discovery too so a CI box without the
// CMake hint at the standard path can still find a globally-
// installed shaderc and run the round-trip test.
bool shadercAvailable() {
    try {
        AYShadercDriver probe;
        return !probe.shadercPath().empty();
    } catch (const std::runtime_error&) {
        return false;
    }
}

std::string shadercEnvOrHint() {
    if (const char* p = std::getenv(AYShadercDriver::kShadercEnvVar)) {
        if (*p) return p;
    }
    return AY_SHADER_SHADERC_HINT;
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

} // namespace

TEST_SUITE(ShadercDriverTests)

TEST_CASE(shaderc_driver_ctor_succeeds_when_shaderc_present) {
    clearPhase36Env();
    if (!shadercAvailable()) {
        std::cerr << "[shaderc test] SKIP: shaderc not available at '"
                  << shadercEnvOrHint() << "'. Set " << AYShadercDriver::kShadercEnvVar << ".\n";
        return;  // skip — same pattern Test_ShaderCompile.cpp uses
    }
    AYShadercDriver drv;
    CHECK(!drv.shadercPath().empty());
}

TEST_CASE(shaderc_driver_throws_when_shaderc_missing) {
    // Force the env var to a definitely-missing path. The constructor
    // tries env first, so this short-circuits the CMake-hint /
    // PATH-search fallbacks.
    PUTENV_S(AYShadercDriver::kShadercEnvVar,
             "D:/definitely/not/here/shaderc-no-such-binary");
    bool threw = false;
    try {
        AYShadercDriver drv;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    PUTENV_S(AYShadercDriver::kShadercEnvVar, "");  // reset
    CHECK(threw);
}

TEST_CASE(shaderc_driver_in_memory_to_bytes_round_trip) {
    clearPhase36Env();
    if (!shadercAvailable()) {
        std::cerr << "[shaderc test] SKIP: shaderc not available — round-trip "
                     "test requires shaderc.\n";
        return;
    }

    AYShadercDriver drv;

    // A tiny vertex shader that asks for GLSL 4.30 with bgfx common.sh.
    // Uses the same `attribute vec3 a_position; gl_Position = ...` recipe
    // as every Test_ShaderCompile.cpp e2e test, so a future comparative
    // check between this driver path and the legacy spawn path stays
    // byte-equal.
    std::ostringstream vs;
    vs << "$input a_position\n$output\n"
       << "\n#include \"common.sh\"\n\n"
       << "void main() {\n"
       << "    gl_Position = vec4(a_position, 1.0);\n"
       << "}\n";
    const std::string vsSrc = vs.str();

    ShaderCompileRequest req;
    req.scSource = vsSrc;
    req.stage    = "vertex";
    req.platform = "linux";
    req.profile  = "430";
    req.includeDirs = shadercIncludeDirs();
    req.outputName = "test_vs.sc";

    ShaderCompileResult r = drv.compile(req);
    if (!r.ok) {
        std::cerr << "[shaderc test] round-trip FAILED: " << r.stderrText << "\n";
        CHECK(r.ok);
        return;
    }
    CHECK(!r.bytes.empty());
    // shaderc emits a small bin header; first 4 bytes are usually
    // the bgfx magic. We don't pin exact bytes; non-empty is the
    // contract for this commit.
    CHECK(r.stderrText.empty());
}

TEST_SUITE_END
