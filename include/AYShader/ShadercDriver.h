#pragma once
// AYShader/ShadercDriver.h — Phase 3.6 (productization)
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
//     as a member) and reuses the shaderc path across calls.
//
// Shaderc path resolution (sign-off 2026-07-01):
//   The user configures the shaderc binary path ONCE per process
//   via `AYShadercDriver::setDefaultExecutable(path)` at engine
//   startup. After that, every AYShadercDriver default-constructed
//   anywhere in the process picks up the same path. There is no
//   auto-discovery (no env-var lookup, no PATH search, no CMake-hint
//   default) — the user must explicitly configure it. The
//   per-construction explicit-path overload remains for tests and
//   multi-driver cases.
//
//   Threading: setDefaultExecutable is a write to a process-wide
//   static. Call it during startup (before any compile thread spins
//   up). Reads from concurrent compile threads are safe — we use a
//   const std::string shared between threads, and the assignment
//   happens-before any subsequent reads (the C++17 std::string
//   assignment is atomic with respect to the std::string::operator==
//   and copy operations used by the driver ctor).
//
// Errors:
//   * Default constructor throws std::runtime_error if no default
//     has been set, or std::invalid_argument if the configured
//     path doesn't point to an existing file.
//   * Explicit-path constructor throws std::invalid_argument on
//     empty / missing path.
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
    // ---- Process-wide configuration ----
    //
    // Set the global default shaderc executable path. After this call,
    // every default-constructed AYShadercDriver in the process uses
    // this path. The path is NOT verified here (so you can configure
    // before the file system is fully online if needed); the
    // default constructor verifies it on first use.
    //
    // One engine → one call at startup. If you call this multiple
    // times, the last call wins. Tests should call
    // `clearDefaultExecutable()` in teardown to avoid leaking state
    // into the next test.
    //
    // Threading: call from startup (single-threaded); subsequent
    // reads are safe from any thread.
    static void setDefaultExecutable(const std::string& path);

    // Drop the global default. Default constructors called after
    // this will throw until `setDefaultExecutable` is called again.
    // Tests use this for clean teardown.
    static void clearDefaultExecutable();

    // True when setDefaultExecutable() has been called and the stored
    // path is non-empty. Does not verify the file exists on disk.
    static bool hasDefaultExecutable();

    // ---- Construction ----
    //
    // Default ctor: uses the path configured via
    // `setDefaultExecutable`. Throws std::runtime_error when no
    // default is set, or std::invalid_argument when the configured
    // path doesn't point to an existing file.
    AYShadercDriver();

    // Explicit-path ctor: bypasses the global default. Throws
    // std::invalid_argument on empty / missing path. Used by tests
    // and by code that needs to drive multiple shaderc binaries
    // (rare — most engines have exactly one).
    explicit AYShadercDriver(const std::string& shadercExecutable);

    // Run one compile. Stages scSource to a temp file, invokes shaderc,
    // reads the resulting .bin into bytes. The temp file is deleted on
    // every path (success / failure / exception).
    ShaderCompileResult compile(const ShaderCompileRequest& req);

    // For diagnostics / tests.
    const std::string&    shadercPath() const { return _shadercPath; }

    ~AYShadercDriver() = default;

private:
    std::string _shadercPath;
};

} // namespace ayt::shader
