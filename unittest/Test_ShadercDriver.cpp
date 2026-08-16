// Test_ShadercDriver.cpp �?Phase 3.6 (revised Commit 4)
//
// Tests AYShadercDriver's explicit-path policy (sign-off 2026-07-01):
//   * setDefaultExecutable / clearDefaultExecutable process-wide state
//   * Default ctor uses the global default; throws on unset / missing
//   * Explicit-path ctor takes its own path; throws on empty / missing
//   * In-memory .sc �?.bin bytes round-trip (skips when shaderc missing)
//
// The pre-Commit-4 "auto-discovery" tests (env var, PATH search,
// CMake hint) are gone �?the driver no longer does any of that.
// The host engine is responsible for resolving the shaderc path
// from its own config and calling setDefaultExecutable at startup.

#include "AYShader/ShadercDriver.h"
#include "AYTest.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#endif

using namespace ayt::shader;

namespace {

// stat()-based file existence check (avoids std::filesystem::exists
// ambiguity that surfaces when the engine's pre-C++17 headers leak
// into this TU).
inline bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// CMake-injected absolute path to the vcpkg shaderc binary.
// Same convention the pre-Phase-3.6 Test_ShaderCompile.cpp used.
// Tests use this to call setDefaultExecutable() so the round-trip
// test can run end-to-end. When the vendored binary doesn't exist
// (CI without bgfx) the round-trip test SKIPs gracefully.
#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
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
#ifdef _WIN32
    _putenv("AY_PHOSKIA_KEEP_SOURCES=");
    _putenv("AY_PHOSKIA_DUMP_SC=");
#else
    unsetenv("AY_PHOSKIA_KEEP_SOURCES");
    unsetenv("AY_PHOSKIA_DUMP_SC");
#endif
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

// ---- setDefaultExecutable / clearDefaultExecutable ----

TEST_CASE(set_default_then_default_ctor_uses_it) {
    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();

    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExists(path)) {
        std::cerr << "[shaderc test] SKIP: configured shaderc not at '"
                  << path << "'.\n";
        return;
    }

    AYShadercDriver::setDefaultExecutable(path);
    AYShadercDriver drv;  // default ctor �?should pick up the default
    CHECK(drv.shadercPath() == path);
    AYShadercDriver::clearDefaultExecutable();
}

TEST_CASE(default_ctor_throws_when_no_default_configured) {
    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();

    bool threw = false;
    try {
        AYShadercDriver drv;  // no default set
    } catch (const std::runtime_error&) {
        threw = true;
    } catch (...) {
        // Driver may throw std::invalid_argument instead on some
        // paths; the contract is "default ctor throws when no
        // default is configured", exception type is intentionally
        // permissive.
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(clear_default_resets_state) {
    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    AYShadercDriver::setDefaultExecutable("C:/some/path/shaderc.exe");
    AYShadercDriver::clearDefaultExecutable();

    bool threw = false;
    try {
        AYShadercDriver drv;
    } catch (...) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(default_ctor_throws_when_default_points_to_missing_file) {
    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    AYShadercDriver::setDefaultExecutable("D:/definitely/not/here/shaderc-no-such-binary");

    bool threw = false;
    try {
        AYShadercDriver drv;
    } catch (const std::exception&) {
        threw = true;
    }
    AYShadercDriver::clearDefaultExecutable();
    CHECK(threw);
}

TEST_CASE(default_ctor_throws_when_default_is_empty_string) {
    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    AYShadercDriver::setDefaultExecutable("");

    bool threw = false;
    try {
        AYShadercDriver drv;
    } catch (const std::exception&) {
        threw = true;
    }
    AYShadercDriver::clearDefaultExecutable();
    CHECK(threw);
}

// ---- Explicit-path ctor ----

TEST_CASE(explicit_path_ctor_uses_given_path) {
    clearPhase36Env();
    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExists(path)) {
        std::cerr << "[shaderc test] SKIP: configured shaderc not at '"
                  << path << "'.\n";
        return;
    }
    AYShadercDriver drv(path);
    CHECK(drv.shadercPath() == path);
}

TEST_CASE(explicit_path_ctor_throws_on_empty) {
    bool threw = false;
    try {
        AYShadercDriver drv("");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(explicit_path_ctor_throws_on_missing_file) {
    bool threw = false;
    try {
        AYShadercDriver drv("D:/definitely/not/here/shaderc-no-such-binary");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- Round-trip (skips when shaderc isn't installed) ----

TEST_CASE(shaderc_driver_in_memory_to_bytes_round_trip) {
    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();

    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExists(path)) {
        std::cerr << "[shaderc test] SKIP: configured shaderc not at '"
                  << path << "' �?round-trip test requires shaderc.\n";
        return;
    }

    // Configure the global default so the test reflects how a real
    // engine uses the driver. (Could also use the explicit-path
    // ctor; both are equivalent here.)
    AYShadercDriver::setDefaultExecutable(path);
    AYShadercDriver drv;

    // A tiny vertex shader that asks for GLSL 4.30 with bgfx common.sh.
    // Same `attribute vec3 a_position; gl_Position = ...` recipe as
    // every Test_ShaderCompile.cpp e2e test, so a future comparative
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
        AYShadercDriver::clearDefaultExecutable();
        return;
    }
    CHECK(!r.bytes.empty());
    // shaderc emits a small bin header; we don't pin exact bytes;
    // non-empty is the contract for this commit.
    CHECK(r.stderrText.empty());
    AYShadercDriver::clearDefaultExecutable();
}

TEST_SUITE_END
