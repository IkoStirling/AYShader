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
// converter change will produce a different one — the env flag is the
// single switch to accept the new baseline.

#include "AYPhoskia.h"
#include "AYTest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

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
// backend's concatenated output (varying.def.sc + vs_ + fs_ fences).
std::string compileToOutput(const std::string& src) {
    Compiler compiler;
    auto result = compiler.compile(src);
    if (!result.success) {
        std::fprintf(stderr, "[golden] compile failed: %s\n",
                     result.errors.empty() ? "?" : result.errors.front().message.c_str());
        return {};
    }
    return result.output;
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

    CHECK(fs::exists(srcPath));
    if (!fs::exists(srcPath)) return;

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
        // PASS — no message body needed; the AYTest framework prints [PASS].
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
TEST_CASE(golden_empty)                  { runOneFixture("empty"); }

TEST_SUITE_END