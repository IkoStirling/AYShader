#pragma once
// AYShadercDriver.h — Phase 3.6 (productization)
//
// Lifts shaderc.exe plumbing that used to live in
// unittest/Test_ShaderCompile.cpp into the production library.
// AYBGFXConverter::compileToBinary() consumes an AYShadercDriver
// instead of spawning shaderc itself, so the frontend never has to
// know about the .sc intermediate.
//
// Design (see design.md §8.4 for the locked API contract):
//   * Caller hands the driver an in-memory .sc string + stage /
//     platform / profile / include dirs.
//   * Driver stages the .sc to a temp file (shaderc wants a `-f` path,
//     not stdin), invokes shaderc.exe / shaderc via CreateProcessW /
//     popen, reads the .bin bytes back into a vector<uint8_t>, and
//     deletes the temp file.
//   * Driver is constructed ONCE per AYBGFXConverter instance (cached
//     as a member) and reuses path discovery across calls.
//
// Errors:
//   * Constructor throws std::runtime_error if shaderc cannot be
//     located (env var / CMake hint / PATH). This is a programmer
//     error to fix at install time and the AYBGFXConverter layer
//     converts it into a CompileResult/CompiledShaderProgram error
//     so the frontend never sees an exception.
//   * compile() returns ok=false + stderrText on shaderc failures;
//     it never throws.

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::shader
{

// One shaderc invocation request. scSource is the in-memory GLSL
// (post-AYBGFXConverter emit); the driver materializes it to a temp
// file before invoking shaderc.
//
// stage values (string — keeps shaderc's CLI surface verbatim):
//   "vertex"   / "fragment"   / "compute"
// platform examples:
//   "linux" / "windows" / "osx" / "android" / "ios" / "asm.js"
// profile: GLSL profile (e.g. "430"). Some stages also accept "120"
//          / "300es" — the string is passed to shaderc verbatim.
struct ShaderCompileRequest {
    std::string                scSource;
    std::string                stage;
    std::string                varyingdefSource;   // empty when unused
    std::string                platform = "linux";
    std::string                profile  = "430";
    std::vector<std::string>   includeDirs;
    std::vector<std::string>   defines;
    std::string                outputName  = "shader";  // diagnostic only
};

struct ShaderCompileResult {
    bool                   ok = false;
    std::vector<uint8_t>   bytes;
    std::string            stderrText;   // for diagnostics; not surfaced to frontend by default
};

class AYShadercDriver
{
public:
    // Locate shaderc; cache the path; throw std::runtime_error if missing.
    // Search order:
    //   1. env var AY_SHADER_SHADERC (user override)
    //   2. CMake-injected compile-time hint AY_SHADER_SHADERC_HINT
    //   3. PATH lookup (`where shaderc.exe` / `command -v shaderc`)
    // If none of those resolve to an existing file, throws.
    AYShadercDriver();

    // Run one compile. Stages scSource to a temp file, invokes shaderc,
    // reads the resulting .bin into bytes. The temp file is deleted on
    // every path (success / failure / exception).
    ShaderCompileResult compile(const ShaderCompileRequest& req);

    // For diagnostics / tests.
    const std::string&    shadercPath() const { return _shadercPath; }

    // Env-var name used in step (1) — exposed for tests that want to
    // clear it via unsetenv without typo'ing the literal.
    static constexpr const char* kShadercEnvVar = "AY_SHADER_SHADERC";

private:
    std::string _shadercPath;
};

} // namespace ayt::shader
