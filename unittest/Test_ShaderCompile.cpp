// ============================================================
// AYShader end-to-end Shader Compilation Test
// ============================================================
//
// Runs the full pipeline:
//   Phoskia source -> AYPhoskia::Compiler -> AYBGFXConverter
//                   -> 3 temp .sc files -> bgfx shaderc.exe
//                   -> 2 .bin files
//
// Each test creates a unique temp directory under the path supplied by
// the environment variable AY_SHADER_TEST_TMPDIR (falls back to
// %TEMP%/ayshader_tests). All artifacts are cleaned up afterwards.
//
// If shaderc.exe is not available at the expected path the tests are
// skipped (returning CHECK(true) with a stderr note) — the suite must
// still pass on machines where bgfx isn't installed.

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYIr.h"
#include "AYTest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

const char* kShadercEnvVar = "AY_SHADER_SHADERC";

// CMake injects an absolute fallback path at configure time
// (PROJECT_SOURCE_DIR + thirdParty/bgfx-install/...). When the project
// is built with CMake this resolves correctly even if the test binary
// is run from a different cwd (e.g. a CI worktree). Without CMake we
// fall through to PATH-based search below.
#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif

// bgfx shader sources need to include `common.sh` (and that file in
// turn includes `bgfx_shader.sh`). CMake injects the bgfx source tree
// path; the strings are empty if bgfx wasn't found at configure time
// (the test then prints a helpful diagnostic and skips the shaderc
// invocation).
#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
#endif
#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT ""
#endif

// Search order for shaderc.exe:
//   1. AY_SHADER_SHADERC env var (user override).
//   2. CMake-injected absolute path (PROJECT_SOURCE_DIR-relative).
//   3. Relative path — only meaningful when cwd is the project root.
//   4. PATH lookup via platform popen shell `command -v` / `where`.
std::string shadercPath() {
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return !p.empty() && std::filesystem::exists(p, ec);
    };

    // 1. env override
    if (const char* p = std::getenv(kShadercEnvVar)) {
        if (exists(p)) return p;
    }

    // 2. CMake hint
    if (exists(AY_SHADER_SHADERC_HINT)) return AY_SHADER_SHADERC_HINT;

    // 3. PATH lookup via shell `where` (Windows) or `command -v` (POSIX).
    //    We don't actually run shaderc here — just locate it. The test
    //    runner then uses the returned path with popen().
#ifdef _WIN32
    FILE* pipe = _popen("where shaderc.exe 2>NUL", "r");
#else
    FILE* pipe = popen("command -v shaderc 2>/dev/null", "r");
#endif
    if (pipe) {
        char buf[1024] = {0};
        if (fgets(buf, sizeof(buf), pipe)) {
            std::string found(buf);
            // strip trailing newline / whitespace / NUL
            while (!found.empty() && (found.back() == '\n' ||
                                      found.back() == '\r' ||
                                      found.back() == ' '  ||
                                      found.back() == '\0')) {
                found.pop_back();
            }
            if (exists(found)) {
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                return found;
            }
        }
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }

    // Final fallback — return CMake hint even if it doesn't exist, so
    // the test can emit a precise diagnostic about what was looked for.
    return AY_SHADER_SHADERC_HINT;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string tempDir() {
    if (const char* p = std::getenv("AY_SHADER_TEST_TMPDIR")) {
        if (*p) return p;
    }
#ifdef _WIN32
    if (const char* tmp = std::getenv("TEMP")) return std::string(tmp) + "\\ayshader_tests";
    return "C:\\Temp\\ayshader_tests";
#else
    return "/tmp/ayshader_tests";
#endif
}

// Returns the bgfx include directories in the order shaderc wants.
// These are empty if the bgfx source tree wasn't located at CMake
// configure time — the test then prints a warning and skips shaderc.
std::vector<std::string> includeDirs() {
    std::vector<std::string> dirs;
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return !p.empty() && std::filesystem::exists(p, ec);
    };
    if (exists(AY_SHADER_BGFX_COMMON_HINT)) dirs.push_back(AY_SHADER_BGFX_COMMON_HINT);
    if (exists(AY_SHADER_BGFX_SRC_HINT))    dirs.push_back(AY_SHADER_BGFX_SRC_HINT);
    return dirs;
}

// Run shaderc with the given args; return its exit code and capture stdout/stderr.
//
// Why not _popen on Windows? _popen on MSVC runs the command through
// `cmd.exe /c "..."`, which mangles the inner quoting when the command
// path or any argument contains spaces (a recurring pain point). We
// instead use CreateProcessW directly so the executable + arguments go
// to the OS untouched. POSIX keeps popen for simplicity.
struct ShaderCResult { int exitCode; std::string output; };

#ifdef _WIN32
ShaderCResult runShaderc(const std::string& shaderc,
                         const std::vector<std::string>& args) {
    ShaderCResult r{-1, ""};

    // Build a Win32 command line: "<exe>" "arg1" "arg2" ...
    // Argument quoting rules: wrap in double quotes; escape embedded
    // double quotes by backslash-escaping. This matches CommandLineToArgvW.
    auto quoteArg = [](const std::string& s) -> std::string {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') {
                // Count preceding backslashes — they need doubling
                // before a quote. For simplicity double every backslash
                // followed by anything (good enough for our paths).
                out += '\\';
            }
            out += c;
        }
        out += '"';
        return out;
    };

    std::string cmdLine = quoteArg(shaderc);
    for (const auto& a : args) {
        cmdLine += ' ';
        cmdLine += quoteArg(a);
    }

    // Create pipe for capturing child's stdout (we redirect stderr → stdout).
    HANDLE hRead = nullptr, hWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        r.output = "CreatePipe failed";
        return r;
    }
    // Ensure the read handle is NOT inherited (so the parent can read it).
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdError = hWrite;
    si.hStdOutput = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};

    // CreateProcessW needs a mutable command-line buffer.
    std::wstring cmdLineW(cmdLine.begin(), cmdLine.end());

    BOOL ok = CreateProcessW(
        /*lpApplicationName*/ nullptr,
        /*lpCommandLine*/     cmdLineW.data(),
        /*lpProcessAttributes*/ nullptr,
        /*lpThreadAttributes*/  nullptr,
        /*bInheritHandles*/   TRUE,
        /*dwCreationFlags*/   0,
        /*lpEnvironment*/     nullptr,
        /*lpCurrentDirectory*/ nullptr,
        /*lpStartupInfo*/     &si,
        /*lpProcessInformation*/ &pi);

    if (!ok) {
        r.output = "CreateProcess failed (error " +
                   std::to_string(GetLastError()) + ") for: " + cmdLine;
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return r;
    }
    CloseHandle(hWrite);  // parent doesn't need the write end

    // Read until child closes its end of the pipe.
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
        r.output.append(buf, buf + got);
    }
    CloseHandle(hRead);

    // Wait for the child to finish.
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    r.exitCode = static_cast<int>(exitCode);
    return r;
}
#else
ShaderCResult runShaderc(const std::string& shaderc,
                         const std::vector<std::string>& args) {
    ShaderCResult r{0, ""};
    std::string cmd = "\"" + shaderc + "\"";
    for (const auto& a : args) {
        cmd += " \"" + a + "\"";
    }
    cmd += " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        r.exitCode = -1;
        r.output = "popen failed";
        return r;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) r.output += buf;
    r.exitCode = pclose(pipe);
    return r;
}
#endif

} // namespace

TEST_SUITE(ShaderCompileTests)

// ===== Minimal valid material: returns red vec4 from fragment =====

TEST_CASE(shaderc_compiles_minimal_unlit) {
    const std::string shaderc = shadercPath();
    // End-to-end is mandatory in Phase 1 closure: if shaderc is missing
    // we want the test to fail loudly so the developer notices — not
    // silently CHECK(true). The diagnostic tells the user exactly how
    // to fix it (set AY_SHADER_SHADERC or run CMake with
    // -DAY_SHADER_SHADERC_PATH=...).
    if (!fileExists(shaderc)) {
        std::cerr << "[shaderc test] FATAL: shaderc not found at '"
                  << shaderc << "'. Set environment variable "
                  << kShadercEnvVar << " or re-run CMake with "
                  << "-DAY_SHADER_SHADERC_PATH=/full/path/to/shaderc\n";
        CHECK(false);
        return;
    }

    const std::string dir = tempDir() + "/minimal_unlit";
    std::filesystem::create_directories(dir);

    const char* src = R"(
        material Unlit {
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
    )";
    Compiler compiler;
    auto compileResult = compiler.compile(src);
    CHECK(compileResult.success);

    AYBGFXConverter conv;
    ir::IRGenerator gen;
    auto ast = conv.convertBGFX(gen.generate(*compiler.parse(
        [&]{
            Lexer lx(src);
            std::vector<Token> tk;
            lx.tokenize(tk);
            return tk;
        }())));
    CHECK(ast.success);
    CHECK(ast.materialFiles.size() == 1);

    const auto& f = ast.materialFiles.front();
    const std::string vsPath  = dir + "/vs_Unlit.sc";
    const std::string fsPath  = dir + "/fs_Unlit.sc";
    const std::string defPath = dir + "/varying.def.sc";
    const std::string vsBin   = dir + "/vs_Unlit.bin";
    const std::string fsBin   = dir + "/fs_Unlit.bin";

    std::ofstream(vsPath)  << f.vs;
    std::ofstream(fsPath)  << f.fs;
    std::ofstream(defPath) << f.varyingDef;

    // Build the shaderc arg list with -i include dirs so common.sh and
    // bgfx_shader.sh resolve.
    auto includes = includeDirs();
    std::vector<std::string> vsArgs = {
        "-f", vsPath, "-o", vsBin,
        "--type", "vertex",
        "--platform", "linux",
        "-p", "120",
    };
    for (const auto& d : includes) {
        vsArgs.push_back("-i");
        vsArgs.push_back(d);
    }
    auto rvs = runShaderc(shaderc, vsArgs);
    if (rvs.exitCode != 0) {
        std::cerr << "[shaderc test] vs compile failed:\n" << rvs.output << "\n";
    }
    CHECK(rvs.exitCode == 0);

    std::vector<std::string> fsArgs = {
        "-f", fsPath, "-o", fsBin,
        "--type", "fragment",
        "--platform", "linux",
        "-p", "120",
        "--varyingdef", defPath,
    };
    for (const auto& d : includes) {
        fsArgs.push_back("-i");
        fsArgs.push_back(d);
    }
    auto rfs = runShaderc(shaderc, fsArgs);
    if (rfs.exitCode != 0) {
        std::cerr << "[shaderc test] fs compile failed:\n" << rfs.output << "\n";
    }
    CHECK(rfs.exitCode == 0);

    CHECK(std::filesystem::file_size(vsBin) > 0);
    CHECK(std::filesystem::file_size(fsBin) > 0);

    std::filesystem::remove_all(dir);
}

// ===== Material with texture sampling =====

TEST_CASE(shaderc_compiles_material_with_texture) {
    const std::string shaderc = shadercPath();
    if (!fileExists(shaderc)) {
        std::cerr << "[shaderc test] FATAL: shaderc not found at '"
                  << shaderc << "'. Set " << kShadercEnvVar
                  << " or re-run CMake with "
                  << "-DAY_SHADER_SHADERC_PATH=/full/path/to/shaderc\n";
        CHECK(false);
        return;
    }

    const std::string dir = tempDir() + "/with_texture";
    std::filesystem::create_directories(dir);

    const char* src = R"(
        material PBR {
            texture2d albedoMap
            vertex {
                in pos : position
                in uv  : texcoord
                return vec4(pos, 1.0)
            }
            fragment {
                in uv : texcoord
                return sample(albedoMap, uv)
            }
        }
    )";
    Compiler compiler;
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    auto ast = compiler.parse(tokens);
    ir::IRGenerator gen;
    AYBGFXConverter conv;
    auto bgfxRes = conv.convertBGFX(gen.generate(*ast));
    CHECK(bgfxRes.success);
    CHECK(bgfxRes.materialFiles.size() == 1);

    const auto& f = bgfxRes.materialFiles.front();
    const std::string vsPath  = dir + "/vs_PBR.sc";
    const std::string fsPath  = dir + "/fs_PBR.sc";
    const std::string defPath = dir + "/varying.def.sc";
    const std::string vsBin   = dir + "/vs_PBR.bin";
    const std::string fsBin   = dir + "/fs_PBR.bin";

    std::ofstream(vsPath)  << f.vs;
    std::ofstream(fsPath)  << f.fs;
    std::ofstream(defPath) << f.varyingDef;

    auto includes = includeDirs();
    std::vector<std::string> vsArgs = {
        "-f", vsPath, "-o", vsBin,
        "--type", "vertex", "--platform", "linux", "-p", "120",
    };
    for (const auto& d : includes) { vsArgs.push_back("-i"); vsArgs.push_back(d); }
    auto rvs = runShaderc(shaderc, vsArgs);
    if (rvs.exitCode != 0) std::cerr << "[shaderc test] vs failed:\n" << rvs.output;
    CHECK(rvs.exitCode == 0);

    std::vector<std::string> fsArgs = {
        "-f", fsPath, "-o", fsBin,
        "--type", "fragment", "--platform", "linux", "-p", "120",
        "--varyingdef", defPath,
    };
    for (const auto& d : includes) { fsArgs.push_back("-i"); fsArgs.push_back(d); }
    auto rfs = runShaderc(shaderc, fsArgs);
    if (rfs.exitCode != 0) std::cerr << "[shaderc test] fs failed:\n" << rfs.output;
    CHECK(rfs.exitCode == 0);

    std::filesystem::remove_all(dir);
}

// ===== Phase 2 closing: end-to-end PBR material =====
//
// A full Cook-Torrance PBR demo with all five PBR builtins (Fresnel-
// Schlick, Fresnel-Schlick-Roughness for IBL/clearcoat, GGX, Schlick-
// GGX, Smith), two texture samples (albedo + normal), two properties
// (emission + envColor for IBL), a [variant useEmission] opt-in code
// path, and a clearcoat specular layer on top of the base BRDF.
//
// The test asserts that the bgfx shaderc toolchain can lower the whole
// pipeline all the way to .bin on at least one target (linux /
// GLSL 1.20 by default).

TEST_CASE(shaderc_compiles_pbr_with_ggx_and_fresnel) {
    const std::string shaderc = shadercPath();
    if (!fileExists(shaderc)) {
        std::cerr << "[pbr test] FATAL: shaderc not found at '" << shaderc << "'.\n";
        CHECK(false);
        return;
    }

    const std::string dir = tempDir() + "/pbr";
    std::filesystem::create_directories(dir);

    // NOTE: this test exercises the FULL Phoskia -> bgfx -> shaderc
    // pipeline using the higher-level PBR builtin forms. The converter
    // inlines each call into the equivalent GLSL math expression at
    // emission time (Phase 2 Step 3 + closing), so users can write
    // idiomatic Phoskia without manually expanding the formulas.
    //
    // The demo also exercises the clearcoat layer and the IBL diffuse
    // term — these are common in production PBR pipelines (glTF /
    // Unreal / Filament) and use the fresnelSchlickRoughness variant
    // of the Fresnel formula to mix the base color with a procedurally
    // sampled environment color (envColor property, a placeholder for
    // a real cubemap lookup).
    const char* src = R"(
        material PBR {
            texture2d albedoMap
            texture2d normalMap
            uniform mat4 modelViewProj
            uniform vec3 cameraPos
            uniform vec3 lightDir
            uniform float roughness
            uniform float metallic
            uniform float clearcoat
            uniform float clearcoatRoughness
            property emission = vec3(0.0, 0.0, 0.0)
            property envColor  = vec3(0.4, 0.45, 0.5)

            vertex {
                in pos    : position
                in nrm    : normal
                in uv     : texcoord
                out worldNormal : normal = vec3(0.0, 0.0, 1.0)
                out uvCoord     : texcoord = vec2(0.0, 0.0)
                return vec4(pos, 1.0)
            }

            fragment {
                in worldNormal : normal
                in uvCoord     : texcoord
                let N = normalize(worldNormal)
                let V = normalize(cameraPos)
                let L = normalize(lightDir)
                let H = normalize(L + V)
                let baseColor = sample(albedoMap, uvCoord)
                let _normalSample = sample(normalMap, uvCoord)
                let NdotV = max(dot(N, V), 0.001)
                let NdotL = max(dot(N, L), 0.0)
                let NdotH = max(dot(N, H), 0.0)
                let VdotH = max(dot(V, H), 0.0)
                let F0 = mix(vec3(0.04), baseColor.rgb, metallic)
                let F = fresnelSchlick(VdotH, F0)
                let Fcc = fresnelSchlickRoughness(VdotH, F0, clearcoatRoughness)
                let D = distributionGGX(NdotH, roughness)
                let G = geometrySmith(NdotV, NdotL, roughness)
                let Gcc = geometrySchlickGGX(NdotV, clearcoatRoughness)
                let specular = D * G * F / max(4.0 * NdotV, 0.001)
                let clearcoatSpec = D * Gcc * Fcc / max(4.0 * NdotV, 0.001) * clearcoat
                let diffuseIBL = baseColor.rgb * envColor * (vec3(1.0) - F) * (1.0 - metallic)
                let diffuse = baseColor.rgb * (vec3(1.0) - F) * (1.0 - metallic) / 3.14159265
                let result = diffuse + diffuseIBL + specular + clearcoatSpec
                [variant useEmission]
                result = result + emission
                return vec4(result, 1.0)
            }
        }
    )";

    Compiler compiler;
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    auto ast = compiler.parse(tokens);
    ir::IRGenerator gen;
    AYBGFXConverter conv;
    auto bgfxRes = conv.convertBGFX(gen.generate(*ast));
    CHECK(bgfxRes.success);
    CHECK(bgfxRes.materialFiles.size() == 1);

    // Sanity-check that the converter registered both textures and
    // all the PBR-related uniforms/properties in the result metadata.
    // This guards against future refactors that drop the ShaderParam /
    // PropertyDecl → uniform registration path silently.
    bool hasAlbedo = false, hasNormal = false;
    for (const auto& t : bgfxRes.textures) {
        if (t.name == "albedoMap") hasAlbedo = true;
        if (t.name == "normalMap") hasNormal = true;
    }
    CHECK(hasAlbedo);
    CHECK(hasNormal);
    int uniformCount = 0;
    bool hasEmission = false, hasEnvColor = false, hasRoughness = false;
    for (const auto& u : bgfxRes.uniforms) {
        ++uniformCount;
        if (u.name == "emission")  hasEmission  = true;
        if (u.name == "envColor")  hasEnvColor  = true;
        if (u.name == "roughness") hasRoughness = true;
    }
    CHECK(uniformCount >= 9);  // 5 user uniforms + 2 properties + clearcoat(2)
    CHECK(hasEmission);
    CHECK(hasEnvColor);
    CHECK(hasRoughness);

    const auto& f = bgfxRes.materialFiles.front();
    const std::string vsPath  = dir + "/vs_PBR.sc";
    const std::string fsPath  = dir + "/fs_PBR.sc";
    const std::string defPath = dir + "/varying.def.sc";
    const std::string vsBin   = dir + "/vs_PBR.bin";
    const std::string fsBin   = dir + "/fs_PBR.bin";

    std::ofstream(vsPath)  << f.vs;
    std::ofstream(fsPath)  << f.fs;
    std::ofstream(defPath) << f.varyingDef;

    auto includes = includeDirs();
    std::vector<std::string> vsArgs = {
        "-f", vsPath, "-o", vsBin,
        "--type", "vertex", "--platform", "linux", "-p", "120",
    };
    for (const auto& d : includes) { vsArgs.push_back("-i"); vsArgs.push_back(d); }
    auto rvs = runShaderc(shaderc, vsArgs);
    if (rvs.exitCode != 0) std::cerr << "[pbr test] vs failed:\n" << rvs.output;
    CHECK(rvs.exitCode == 0);
    if (rvs.exitCode != 0) {
        std::filesystem::remove_all(dir);
        return;
    }

    std::vector<std::string> fsArgs = {
        "-f", fsPath, "-o", fsBin,
        "--type", "fragment", "--platform", "linux", "-p", "120",
        "--varyingdef", defPath,
    };
    for (const auto& d : includes) { fsArgs.push_back("-i"); fsArgs.push_back(d); }
    auto rfs = runShaderc(shaderc, fsArgs);
    if (rfs.exitCode != 0) std::cerr << "[pbr test] fs failed:\n" << rfs.output;
    CHECK(rfs.exitCode == 0);
    if (rfs.exitCode != 0) {
        std::filesystem::remove_all(dir);
        return;
    }

    // The .bin artifacts must be non-empty. Skipped if shaderc failed
    // (fsBin won't exist) — that's already reported via the exitCode
    // CHECK above.
    CHECK(std::filesystem::file_size(vsBin) > 0);
    CHECK(std::filesystem::file_size(fsBin) > 0);

    std::filesystem::remove_all(dir);
}

TEST_SUITE_END
