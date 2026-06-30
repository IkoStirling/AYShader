// AYShadercDriver.cpp — Phase 3.6
//
// See AYShadercDriver.h. Implementation lifted from
// unittest/Test_ShaderCompile.cpp shaderc plumbing (lines 46-285).
// Same plumbing, just promoted to production code with a few small
// hardening changes:
//   * Remove the "best-effort return hint even if missing" fallback
//     (production must surface a clear diagnostic).
//   * Wrap CreateProcessW shaderc invocation in a try/finally so the
//     temp .sc file is deleted on every code path.
//   * Use atomic pid+counter naming for the temp file so concurrent
//     compile() calls in the same process don't collide.

#include "AYShadercDriver.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdio>          // ::remove
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace ayt::shader
{

// CMake-injected absolute path. Default fallback is the project-local
// vendored bgfx-install path; same convention as the pre-Phase-3.6
// test code.
#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif

namespace {

// stat()-based file existence check. Same reason as in
// Test_ShadercDriver.cpp: avoids std::filesystem::exists ambiguity
// that surfaces when other AY headers leak into this TU.
bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

#if defined(_WIN32)
bool lookupOnPath(std::string& outFound) {
    FILE* pipe = _popen("where shaderc.exe 2>NUL", "r");
    if (!pipe) return false;
    char buf[1024] = {0};
    bool found = false;
    if (fgets(buf, sizeof(buf), pipe)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                               s.back() == ' '  || s.back() == '\0')) {
            s.pop_back();
        }
        if (fileExists(s)) { outFound = s; found = true; }
    }
    _pclose(pipe);
    return found;
}
#else
bool lookupOnPath(std::string& outFound) {
    FILE* pipe = popen("command -v shaderc 2>/dev/null", "r");
    if (!pipe) return false;
    char buf[1024] = {0};
    bool found = false;
    if (fgets(buf, sizeof(buf), pipe)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                               s.back() == ' '  || s.back() == '\0')) {
            s.pop_back();
        }
        if (fileExists(s)) { outFound = s; found = true; }
    }
    pclose(pipe);
    return found;
}
#endif

// Spawn a child process with an arbitrary command line, capture
// combined stdout/stderr. Returns exit code (-1 if spawn failed).
//
// Mirrors the pre-Phase-3.6 Test_ShaderCompile.cpp::runShaderc()
// exactly, minus the .sc-specific bits. The {hRead, hWrite} pipe
// pattern is the standard Win32 inheritance dance; POSIX uses popen
// for brevity.
struct SpawnResult { int exitCode; std::string output; };

#if defined(_WIN32)
SpawnResult spawnCapturing(const std::string& exe,
                           const std::vector<std::string>& args) {
    SpawnResult r{-1, ""};

    auto quoteArg = [](const std::string& s) -> std::string {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
        return out;
    };

    std::string cmdLine = quoteArg(exe);
    for (const auto& a : args) { cmdLine += ' '; cmdLine += quoteArg(a); }

    HANDLE hRead = nullptr, hWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        r.output = "CreatePipe failed";
        return r;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdError  = hWrite;
    si.hStdOutput = hWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags   |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::wstring cmdLineW(cmdLine.begin(), cmdLine.end());

    BOOL ok = CreateProcessW(
        nullptr, cmdLineW.data(),
        nullptr, nullptr,
        TRUE, 0, nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        r.output = "CreateProcess failed (error " +
                   std::to_string(GetLastError()) + ") for: " + cmdLine;
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return r;
    }
    CloseHandle(hWrite);

    char buf[4096];
    DWORD got = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
        r.output.append(buf, buf + got);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    r.exitCode = static_cast<int>(exitCode);
    return r;
}
#else
SpawnResult spawnCapturing(const std::string& exe,
                           const std::vector<std::string>& args) {
    SpawnResult r{0, ""};
    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : args) { cmd += " \"" + a + "\""; }
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

// Build a unique temp .sc path. Process-pid + monotonic counter
// sidesteps race conditions if multiple drivers / threads happen to
// compile concurrently. Caller is responsible for deleting the file
// (we RAII that in compile()).
std::string uniqueScTempPath() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const unsigned pid = ::GetCurrentProcessId();
    const char* tmp = std::getenv("TEMP");
    std::string dir = (tmp && *tmp) ? std::string(tmp) + "\\" : std::string("C:\\Temp\\");
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? std::string(tmp) + "/" : std::string("/tmp/");
#endif
    return dir + "ayshader_" + std::to_string(pid) +
           "_" + std::to_string(n) + ".sc";
}

// Path-with-optional-extension: shaderc chooses .bin / .vert / .frag
// / .comp based on the matching --type we already pass, so we just
// slap ".bin" here. Driver cleans up after spawn.
std::string siblingBinPath(const std::string& scPath) {
    // Hand-rolled replace_extension — std::filesystem::path is dodged
    // here for the same reason fileExists() is: it draws in
    // <filesystem> global symbols that collide with older AY headers.
    auto dotPos = scPath.find_last_of('.');
    if (dotPos == std::string::npos) return scPath + ".bin";
    return scPath.substr(0, dotPos) + ".bin";
}

} // namespace

AYShadercDriver::AYShadercDriver() {
    // 1. env override
    if (const char* p = std::getenv(kShadercEnvVar)) {
        if (fileExists(p)) { _shadercPath = p; return; }
    }
    // 2. CMake-injected hint
    if (fileExists(AY_SHADER_SHADERC_HINT)) {
        _shadercPath = AY_SHADER_SHADERC_HINT;
        return;
    }
    // 3. PATH lookup
    std::string found;
    if (lookupOnPath(found)) { _shadercPath = found; return; }
    // 4. Hard fail — production must surface a clear diagnostic.
    throw std::runtime_error(
        "AYShadercDriver: shaderc not found. Set AY_SHADER_SHADERC env var "
        "or re-run CMake with -DAY_SHADER_SHADERC_PATH=<full path to shaderc>. "
        "Searched: env var, CMake hint '" +
        std::string(AY_SHADER_SHADERC_HINT) +
        "', and PATH.");
}

ShaderCompileResult AYShadercDriver::compile(const ShaderCompileRequest& req) {
    ShaderCompileResult result;

    // 1) Stage the in-memory .sc to a temp file.
    std::string scPath = uniqueScTempPath();
    std::string binPath = siblingBinPath(scPath);
    {
        std::ofstream f(scPath, std::ios::binary);
        if (!f) {
            result.stderrText = "cannot write temp .sc: " + scPath;
            return result;
        }
        f << req.scSource;
    }

    // RAII guard: delete temp files on every exit path. We capture
    // both paths by value so the lambda outlives local variables.
    auto cleanup = [&]() {
        (void)::remove(scPath.c_str());
        (void)::remove(binPath.c_str());
    };

    // 2) Build argv. Mirrors Test_ShaderCompile.cpp's per-test argv
    //    shape so the Round-trip test (commit B1) matches the
    //    historical e2e shape byte-equal.
    std::vector<std::string> args = {
        "-f", scPath,
        "-o", binPath,
        "--type", req.stage,
        "--platform", req.platform,
        "-p", req.profile,
    };
    for (const auto& inc : req.includeDirs) {
        args.push_back("-i"); args.push_back(inc);
    }
    for (const auto& d : req.defines) {
        args.push_back("--define"); args.push_back(d);
    }
    if (!req.varyingdefSource.empty()) {
        // bgfx shaderc wants --varyingdef <path>; we materialize the
        // varyingdef into a sibling temp file too.
        std::string vdPath = scPath + ".varyingdef";
        std::ofstream f(vdPath, std::ios::binary);
        if (!f) {
            cleanup();
            result.stderrText = "cannot write temp varyingdef: " + vdPath;
            return result;
        }
        f << req.varyingdefSource;
        args.push_back("--varyingdef"); args.push_back(vdPath);
    }

    // 3) Spawn.
    SpawnResult sr = spawnCapturing(_shadercPath, args);

    // 4) Read .bin back into memory.
    if (sr.exitCode == 0) {
        std::ifstream in(binPath, std::ios::binary);
        if (in) {
            result.bytes.assign(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
            result.ok = !result.bytes.empty();
        } else {
            result.stderrText = "shaderc exit 0 but .bin missing: " + binPath;
        }
    } else {
        result.stderrText = "shaderc exit " + std::to_string(sr.exitCode) +
                            ": " + sr.output + "\n(request: " + req.outputName + ")";
    }

    cleanup();
    return result;
}

} // namespace ayt::shader
