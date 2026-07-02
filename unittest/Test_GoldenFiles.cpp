// ============================================================
// AYShader Golden File Verification (Phase 2 closing)
// ============================================================
//
// Verifies that the BGFX backend converter produces byte-identical
// .sc output for a curated set of Phoskia fixtures. Each fixture lives
// in unittest/golden/<name>.phoskia; the expected output lives next to
// it as <name>.sc.
//
// Workflow:
//   1. First run (no .sc file present)  -> generates <name>.sc from
//      the Phoskia source. Subsequent runs compare byte-equal.
//   2. AY_SHADER_REGEN_GOLDEN=1 in env  -> unconditionally regenerates
//      the .sc file (used after intentional converter changes).
//
// Each test names the exact mismatch on failure so regenerating is
// safe: a typo in source will reproduce the exact same .sc, and a
// converter change will produce a different one ??the env flag is the
// single switch to accept the new baseline.

#include "AYPhoskia.h"
#include "AYShadercDriver.h"
#include "AYTest.h"
#include "detail/AYShaderSourceKeys.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

// Lock the process-wide shaderc default for `compileToOutput`. The
// golden joiner doesn't actually need shaderc ??it reconstructs the
// shape from `prog.sources` which is populated pre-shaderc ??but
// `compileToProgram` lazily initializes the driver from the global
// default and surfaces a "no default executable configured" error
// when unset. Call `setDefaultExecutable` once so the golden path
// is order-independent from Test_ShadercDriver.cpp /
// Test_CompileToBinary.cpp.
#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif

inline bool fileExistsGolden(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

void ensureShadercDefaultForGolden() {
    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExistsGolden(path)) return;
    try {
        ayt::shader::AYShadercDriver probe(path);
        ayt::shader::AYShadercDriver::setDefaultExecutable(path);
    } catch (...) {
        // Best-effort: golden tests don't depend on shaderc working.
    }
}

// Locate the unittest/golden directory. CMake copies the golden folder
// next to the test source so a relative path resolves correctly both
// during in-source builds and after install. We also try a couple of
// fallback locations for robustness.
std::string goldenDir() {
    const char* candidates[] = {
        "unittest/golden",                // run from project root
        "../unittest/golden",             // run from out/build/<cfg>/...
        "../../unittest/golden",          // run from out/build/<cfg>/<lib>/...
        "golden",                         // cd'd into unittest/
    };
    for (const char* c : candidates) {
        if (fs::exists(c) && fs::is_directory(c)) return c;
    }
    return "unittest/golden";  // best-effort default
}

bool regenMode() {
    const char* env = std::getenv("AY_SHADER_REGEN_GOLDEN");
    return env && env[0] && env[0] != '0';
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

// Run the Phoskia source through the full pipeline and return the
// backend's concatenated output (varying_definitions + vs_ + fs_ fences).
//
// Phase 3.6 Commit 5: the new pipeline populates
// `CompiledShaderProgram::sources` instead of
// `CompileResult::output` (the latter is now deprecated / empty by
// default). To keep the golden .sc baselines byte-equal against the
// historical fence-prefixed joiner, we reconstruct the same shape
// from `prog.sources` here. This is a test-only joiner ??production
// `CompiledShaderProgram::sources` stays a map and the frontend is
// free to use whatever structure it wants.
//
// Key conventions (kept stable; locked in design.md ?8.4):
//   "vs_<i>.sc"          - vertex stage for material i
//   "fs_<i>.sc"          - fragment stage for material i
//   "cs_<i>.sc"          - compute stage for compute i
//   "varying_definitions"     - shared varying/attribute table (single key)
//
// Joins:
//   For each material i:
//     // === material <i> varying_definitions ===
//     <varying.def sc>     (only the FIRST material emits varyingdef; later
//                            materials share the same per-program varyingdef)
//     // === material <i> vs ===
//     <vs>
//     // === material <i> fs ===
//     <fs>
//   For each compute i:
//     // === compute <i> cs ===
//     <cs>
//
// (The historical joiner emitted `// === material 0 varying_definitions ===` only
// for material 0 ??subsequent materials' varying.def was identical and not
// repeated.)
std::string compileToOutput(const std::string& src) {
    Compiler compiler;
    CompileOptions opts;
    opts.keepSources = true;
    ensureShadercDefaultForGolden();
    // Isolate env-var influence on the joiner ??envs could otherwise
    // toggle keepSources at runtime, which is fine for production
    // but would make this baseline-diff test order-dependent.
#ifdef _WIN32
    _putenv("AY_PHOSKIA_KEEP_SOURCES=");
    _putenv("AY_PHOSKIA_DUMP_SC=");
#else
    unsetenv("AY_PHOSKIA_KEEP_SOURCES");
    unsetenv("AY_PHOSKIA_DUMP_SC");
#endif
    CompiledShaderProgram prog = compiler.compileToProgram(src, opts);
    if (!prog.success) {
        std::fprintf(stderr, "[golden] compileToProgram failed: %s\n",
                     prog.errors.empty() ? "?" : prog.errors.front().c_str());
        return {};
    }

    // Reconstruct the historical multi-fence joiner from prog.sources.
    // Empty keys indicate "no stage in this slot".
    //
    // Note: the historical joiner (see AYBGFXConverter.cpp::convert())
    // repeated `// === material <i> varying_definitions ===` per material ??    // i.e. material 0's varyingdef appears once, material 1's appears
    // once (even if it's the same string as material 0's), etc. The
    // sources map only carries varying.def under a single shared
    // key; we re-emit it per material so the golden .sc baseline
    // remains byte-equal.
    std::ostringstream oss;
    const std::string varyingDef =
        prog.sources.count("varying_definitions")
            ? prog.sources.at("varying_definitions") : std::string{};
    using detail::computeStageKey;
    using detail::fragmentStageKey;
    using detail::vertexStageKey;
    for (size_t i = 0; ; ++i) {
        const std::string vsKey = vertexStageKey(i);
        const std::string fsKey = fragmentStageKey(i);
        if (!prog.sources.count(vsKey)) break;  // past last material
        oss << "// === material " << i << " varying.def.sc ===\n"
            << varyingDef << "\n";
        oss << "// === material " << i << " vs ===\n"
            << prog.sources.at(vsKey) << "\n";
        oss << "// === material " << i << " fs ===\n"
            << prog.sources.at(fsKey) << "\n";
    }
    for (size_t i = 0; ; ++i) {
        const std::string csKey = computeStageKey(i);
        if (!prog.sources.count(csKey)) break;
        oss << "// === compute " << i << " cs ===\n"
            << prog.sources.at(csKey) << "\n";
    }
    return oss.str();
}

// Find the byte index of the first divergence between actual and
// expected, for diagnostic output. Returns -1 if equal.
ptrdiff_t firstDiff(const std::string& actual, const std::string& expected) {
    size_t n = std::min(actual.size(), expected.size());
    for (size_t i = 0; i < n; ++i) {
        if (actual[i] != expected[i]) return static_cast<ptrdiff_t>(i);
    }
    if (actual.size() != expected.size()) {
        return static_cast<ptrdiff_t>(n);
    }
    return -1;
}

void runOneFixture(const std::string& name) {
    std::string dir = goldenDir();
    std::string srcPath = dir + "/" + name + ".phoskia";
    std::string outPath = dir + "/" + name + ".sc";

    if (!fs::exists(srcPath)) {
        std::fprintf(stderr,
                     "SKIP golden_%s: missing fixture '%s' (set AY_SHADER_REGEN_GOLDEN=1 after adding fixtures)\n",
                     name.c_str(), srcPath.c_str());
        return;
    }

    std::string src = readFile(srcPath);
    CHECK_FALSE(src.empty());
    if (src.empty()) return;

    std::string actual = compileToOutput(src);
    CHECK_FALSE(actual.empty());
    if (actual.empty()) return;

    if (!fs::exists(outPath) || regenMode()) {
        writeFile(outPath, actual);
        std::printf("  [GEN] wrote golden baseline %s (%zu bytes)\n",
                    outPath.c_str(), actual.size());
        return;
    }

    std::string expected = readFile(outPath);
    if (actual == expected) {
        // PASS ??no message body needed; the AYTest framework prints [PASS].
        return;
    }

    // Surface the first divergence for fast diagnosis.
    auto at = firstDiff(actual, expected);
    std::printf("  [DIFF] %s differs at byte %td\n", name.c_str(), at);
    if (at >= 0) {
        size_t i = static_cast<size_t>(at);
        size_t ctx = 60;
        size_t beg = (i > ctx) ? i - ctx : 0;
        std::printf("    actual:   ...%s\n",
                    actual.substr(beg, std::min(ctx * 2, actual.size() - beg)).c_str());
        std::printf("    expected: ...%s\n",
                    expected.substr(beg, std::min(ctx * 2, expected.size() - beg)).c_str());
    }
    CHECK(actual == expected);
}

}  // namespace

TEST_SUITE(GoldenFilesTests)

TEST_CASE(golden_unlit)                  { runOneFixture("unlit"); }
TEST_CASE(golden_pbr_minimal)            { runOneFixture("pbr_minimal"); }
TEST_CASE(golden_pbr_with_emission)      { runOneFixture("pbr_with_emission"); }
TEST_CASE(golden_pbr_with_texture)       { runOneFixture("pbr_with_texture"); }
TEST_CASE(golden_pbr_full)               { runOneFixture("pbr_full"); }
TEST_CASE(golden_empty)                  { runOneFixture("empty"); }
// Phase 3.2 Block 4: compute fixture exercises Blocks 1-3 end-to-end
// (convertComputeDecl skeleton + thread_id inline + storage buffer
// emission). The .sc baseline is auto-generated on first run if
// missing; regenerate with AY_SHADER_REGEN_GOLDEN=1 after an
// intentional converter change.
TEST_CASE(golden_compute_minimal)        { runOneFixture("compute_minimal"); }
// Phase 3.5-A: compute fixture exercising storage decl explicit
// binding slots. Two storage buffers with explicit bindings 0 and 1;
// the BGFX backend emits
//   `layout(std430, binding = 0) buffer inputs { float data[]; } inputs;`
//   `layout(std430, binding = 1) buffer outputs { float data[]; } outputs;`
// in the cs output. The .sc baseline is auto-generated on first run if
// missing; regenerate with AY_SHADER_REGEN_GOLDEN=1 after an
// intentional converter change.
TEST_CASE(golden_compute_with_storage_binding) {
    runOneFixture("compute_with_storage_binding");
}
// Phase 3.5-B: material fixture exercising uniformblock explicit
// binding slots. Camera has no binding (auto slot 0), Lighting has
// explicit `binding 3`; the BGFX backend emits
//   `layout(std140, binding = 0) uniform Camera { ... } Camera;`
//   `layout(std140, binding = 3) uniform Lighting { ... } Lighting;`
// spliced into both vs and fs. The .sc baseline is auto-generated on
// first run if missing; regenerate with AY_SHADER_REGEN_GOLDEN=1
// after an intentional converter change.
TEST_CASE(golden_material_with_ubo_binding) {
    runOneFixture("material_with_ubo_binding");
}

TEST_SUITE_END