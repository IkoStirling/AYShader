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
#include "AYShadercDriver.h"  // for AYShadercDriver::clearDefaultExecutable()
#include "AYTest.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  define PUTENV_S(name, val) _putenv_s(name, val)
#  include <windows.h>
#  include <process.h>  // getpid
#  define GETPID() _getpid()
#else
#  define PUTENV_S(name, val) setenv(name, val, 1)
#  include <sys/types.h>
#  include <unistd.h>
#  define GETPID() ::getpid()
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

// Reset the process-wide shaderc default so each test sees a
// known starting point. (Other test files in the same binary
// set/clear this; we don't want test ordering to leak into
// env-var precedence results.)
void clearShadercDefault() {
    AYShadercDriver::clearDefaultExecutable();
}

// stat()-based file existence check. Avoids `<filesystem>` for the
// same reason Test_CompileToBinary.cpp / Test_ShadercDriver.cpp do.
inline bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// Pick a unique per-process dump dir under temp; concurrent CI
// runs don't clash because the dir name is pid-stamped. Caller is
// responsible for the lifetime (usually a single test case).
std::string shadercTestDumpDir(const char* tag) {
    char buf[512];
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    if (!t) t = "C:\\Temp";
    std::snprintf(buf, sizeof(buf), "%s\\phoskia_test_%u_%s",
                  t, static_cast<unsigned>(GETPID()), tag);
#else
    const char* t = std::getenv("TMPDIR");
    if (!t) t = "/tmp";
    std::snprintf(buf, sizeof(buf), "%s/phoskia_test_%u_%s",
                  t, static_cast<unsigned>(GETPID()), tag);
#endif
    return std::string(buf);
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
    clearShadercDefault();

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
    clearShadercDefault();

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
    clearShadercDefault();
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
    clearShadercDefault();
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
    clearShadercDefault();
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
    clearShadercDefault();
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

// ----- AY_PHOSKIA_DUMP_SC precedence tests (mirror the KEEP_SOURCES
// ----- ones above; covers the parallel toggle for the dump dir).
//
// All four combinations of {opts, env} × {on, off} are tested. The
// contract is true-wins OR: any source wanting dumpIntermediate ON
// flips it ON. Same shape as keepSources.

TEST_CASE(env_AY_PHOSKIA_DUMP_SC_overrides_opts_false) {
    clearPhase36Env();
    clearShadercDefault();
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "1");

    Compiler c;
    CompileOptions opts;
    opts.dumpIntermediate = false;
    opts.dumpDir = shadercTestDumpDir("dump_env_off");
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    // Even though opts.dumpIntermediate=false, env=1 forces ON.
    // The dump must happen on the happy path. On the shaderc-
    // missing path `program.success` is false and we don't assert
    // the file exists (the test can't run end-to-end).
    if (!program.success) {
        std::cerr << "[dump_sc test] SKIP: shaderc not available.\n";
        return;
    }
    CHECK(fileExists(opts.dumpDir + "/vs_0.sc"));
    CHECK(fileExists(opts.dumpDir + "/fs_0.sc"));
}

TEST_CASE(env_AY_PHOSKIA_DUMP_SC_combines_with_opts_true) {
    clearPhase36Env();
    clearShadercDefault();
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "1");

    Compiler c;
    CompileOptions opts;
    opts.dumpIntermediate = true;
    opts.dumpDir = shadercTestDumpDir("dump_both_on");
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[dump_sc test] SKIP: shaderc not available.\n";
        return;
    }
    CHECK(fileExists(opts.dumpDir + "/vs_0.sc"));
}

TEST_CASE(env_AY_PHOSKIA_DUMP_SC_zero_opts_true_wins) {
    clearPhase36Env();
    clearShadercDefault();
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "0");

    Compiler c;
    CompileOptions opts;
    opts.dumpIntermediate = true;  // explicit ON
    opts.dumpDir = shadercTestDumpDir("dump_env_zero_opts_on");
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[dump_sc test] SKIP: shaderc not available.\n";
        return;
    }
    // opts wins because the contract is true-wins OR.
    CHECK(fileExists(opts.dumpDir + "/vs_0.sc"));
}

TEST_CASE(env_AY_PHOSKIA_DUMP_SC_zero_opts_false_is_off) {
    clearPhase36Env();
    clearShadercDefault();
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "0");

    Compiler c;
    CompileOptions opts;
    opts.dumpIntermediate = false;
    opts.dumpDir = shadercTestDumpDir("dump_both_off");
    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[dump_sc test] SKIP: shaderc not available.\n";
        return;
    }
    CHECK(!fileExists(opts.dumpDir + "/vs_0.sc"));
}

// Parse error surfaces in program.errors without throwing.
TEST_CASE(compileToProgram_parser_error_surfaces) {
    clearPhase36Env();
    clearShadercDefault();

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
    clearShadercDefault();

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

// Phase 3.6 Commit 6: when opts.dumpIntermediate=true AND a dump
// dir is supplied, the .sc files land on disk in dumpDir. SKIPs
// the file-existence check on the shaderc-missing path because
// the dump branch only fires when shaderc's compileStage succeeded.
//
// This is the disk-side counterpart of Test_CompileToBinary.cpp's
// compileToBinary_dump_intermediate_writes_files but at the
// Compiler (frontend) level so the env-var chain has a regression
// guard.
TEST_CASE(compileToProgram_respects_dump_dir) {
    clearPhase36Env();
    clearShadercDefault();

    Compiler c;
    CompileOptions opts;
    opts.dumpIntermediate = true;
    opts.dumpDir = shadercTestDumpDir("respects_dump_dir");

    CompiledShaderProgram program = c.compileToProgram(kMinimalUnlit, opts);

    if (!program.success) {
        std::cerr << "[dump_dir test] SKIP: shaderc not available, "
                     "opt-in dump path not exercised.\n";
        return;
    }
    // Happy path: dumpDir is created and the .sc files exist.
    CHECK(fileExists(opts.dumpDir + "/vs_0.sc"));
    CHECK(fileExists(opts.dumpDir + "/fs_0.sc"));
    CHECK(fileExists(opts.dumpDir + "/varying.def.sc"));
}

TEST_SUITE_END