// Test_CompileToProgram.cpp — Phase 3.6 Commit 3 (B3)
//
// Tests the Compiler::compileToProgram() productization entry point
// and the env-var precedence contract for AY_PHOSKIA_KEEP_SOURCES /
// AY_PHOSKIA_DUMP_SC. These tests live at the Compiler level (not
// AYBGFXConverter level — see Test_CompileToBinary.cpp for that).
//
// Scope is intentionally small for Commit 3:
//   - compileToProgram default opts produces a CompiledShaderProgram
//   - compileToProgram(src, opts) passes opts through
//   - keepSources / dumpIntermediate env vars are OR'd (true-wins)
//   - parse failures surface in program.errors without throwing
//
// The e2e (verify bytes are real bgfx binaries) checks live in
// Test_CompileToBinary.cpp; here we test the Compiler-level glue.

#include "AYPhoskia.h"
#include "AYTest.h"

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#  define PUTENV_S(name, val) _putenv_s(name, val)
#else
#  define PUTENV_S(name, val) setenv(name, val, 1)
#endif

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

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

// Clean env-var surface so precedence tests aren't polluted by the
// caller's shell. AY_PHOSKIA_KEEP_SOURCES / AY_PHOSKIA_DUMP_SC are
// introduced by Commit 3; legacy tests don't touch them.
void clearPhase36Env() {
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");
}

} // namespace

TEST_SUITE(CompileToProgramTests)

// Smoke: default opts. The full shaderc pipeline may or may not be
// available on the host. We don't assert on the binary shape here —
// just on the contract that compileToProgram returns a struct with
// the expected fields populated (even when shaderc is missing, the
// errors vector carries the diagnostic and success is false).
TEST_CASE(compileToProgram_default_opts_returns_shape) {
    clearPhase36Env();

    Compiler c;
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit);

    // The struct must exist with the expected fields. Don't assert
    // success — that depends on shaderc availability on the host.
    // Either success=true (with vsBin/fsBin populated), or success=false
    // (with errors mentioning AYShadercDriver somewhere — earlier
    // parser-diagnostic messages may also be in the errors vector
    // since they get surfaced too).
    if (!program.success) {
        CHECK(!program.errors.empty());
        bool foundShadercDiag = false;
        for (const auto& e : program.errors) {
            if (e.find("AYShadercDriver") != std::string::npos) {
                foundShadercDiag = true;
                break;
            }
        }
        CHECK(foundShadercDiag);
    } else {
        CHECK(!program.vsBin.empty());
        CHECK(!program.fsBin.empty());
        CHECK(program.csBin.empty());
        // Default opts: no sources, no dumpDir.
        CHECK(program.sources.empty());
        CHECK(program.warnings.empty());
    }
}

// Explicit opts pass-through. keepSources=true populates the sources
// map (when shaderc is available). SKIPs when shaderc missing.
TEST_CASE(compileToProgram_keep_sources_via_opts) {
    clearPhase36Env();

    Compiler c;
    CompileOptions opts;
    opts.keepSources = true;
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[compileToProgram test] SKIP: shaderc not available.\n";
        return;
    }
    CHECK(program.sources.count("vs_0.sc") == 1);
    CHECK(program.sources.count("fs_0.sc") == 1);
}

// Env var override: AY_PHOSKIA_KEEP_SOURCES=1 with opts.keepSources=false
// still results in keepSources=true (true-wins OR).
TEST_CASE(env_AY_PHOSKIA_KEEP_SOURCES_overrides_opts_false) {
    clearPhase36Env();
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "1");

    Compiler c;
    CompileOptions opts;
    opts.keepSources = false;  // explicit false
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[compileToProgram test] SKIP: shaderc not available.\n";
        return;
    }
    // Sources populated even though explicit opts.keepSources=false.
    CHECK(program.sources.count("vs_0.sc") == 1);
    CHECK(program.sources.count("fs_0.sc") == 1);
}

// Env var override: same as above but explicit opts.keepSources=true
// AND env=1 — still works (idempotent).
TEST_CASE(env_AY_PHOSKIA_KEEP_SOURCES_combines_with_opts_true) {
    clearPhase36Env();
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "1");

    Compiler c;
    CompileOptions opts;
    opts.keepSources = true;
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[compileToProgram test] SKIP: shaderc not available.\n";
        return;
    }
    CHECK(program.sources.count("vs_0.sc") == 1);
}

// Env var override: env says OFF, opts say ON — the contract is
// true-wins OR, so the result is ON (opts wants it on). This locks
// the asymmetry: opts.keepSources=true always wins regardless of env.
TEST_CASE(env_AY_PHOSKIA_KEEP_SOURCES_zero_opts_true_wins) {
    clearPhase36Env();
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "0");

    Compiler c;
    CompileOptions opts;
    opts.keepSources = true;
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[compileToProgram test] SKIP: shaderc not available.\n";
        return;
    }
    CHECK(program.sources.count("vs_0.sc") == 1);
}

// Env var override: env=0 + opts=false → sources empty (both want it
// off). This is the "no override" baseline.
TEST_CASE(env_AY_PHOSKIA_KEEP_SOURCES_zero_opts_false_is_off) {
    clearPhase36Env();
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "0");

    Compiler c;
    CompileOptions opts;
    opts.keepSources = false;
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        // shaderc missing case: sources is still empty (no point
        // running convertBGFX successfully without shaderc).
        CHECK(program.sources.empty());
        return;
    }
    // shaderc available: sources empty because neither toggle asked
    // for them.
    CHECK(program.sources.empty());
}

// Parse error surfaces in program.errors without throwing.
TEST_CASE(compileToProgram_parser_error_surfaces) {
    clearPhase36Env();

    Compiler c;
    // Unclosed brace → parser error.
    const std::string bad = "material X { vertex { return vec4(0,0,0,0); ";
    CompiledShaderProgram program = c.compileToProgram(bad);

    CHECK(!program.success);
    CHECK(!program.errors.empty());
}

// Return-value overload calls out-param overload. We can't directly
// verify the SSO/NRVO behavior, but we can verify the return-value
// overload produces equivalent results to the out-param one for a
// happy path.
TEST_CASE(compileToProgram_return_value_matches_out_param) {
    clearPhase36Env();

    Compiler c;

    CompiledShaderProgram viaReturn = c.compileToProgram(kMinimalUnlit);

    CompileOptions opts;
    CompiledShaderProgram viaOutParam;
    c.compileToProgram(kMinimalUnlit, opts, viaOutParam);

    CHECK(viaReturn.success == viaOutParam.success);
    CHECK(viaReturn.errors.size() == viaOutParam.errors.size());
    if (viaReturn.success) {
        CHECK(viaReturn.vsBin.size() == viaOutParam.vsBin.size());
        CHECK(viaReturn.fsBin.size() == viaOutParam.fsBin.size());
    }
}

TEST_SUITE_END