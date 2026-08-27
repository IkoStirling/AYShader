// AYShadercDriver.cpp — Phase 3.6 + AYIO migration
//
// See AYShader/ShadercDriver.h. Implementation lifted from
// unittest/Test_ShaderCompile.cpp shaderc plumbing (lines 46-285).
// Same plumbing, just promoted to production code with a few small
// hardening changes:
//   * No auto-discovery (sign-off 2026-07-01) — driver takes an
//     explicit shaderc path at construction time; the host engine
//     is responsible for resolving the path from its own config.
//   * Wrap CreateProcessW shaderc invocation in a try/finally so the
//     temp .sc file is deleted on every code path.
//   * Use atomic pid+counter naming for the temp file so concurrent
//     compile() calls in the same process don't collide.
//
// AYIO migration (per design.md §16):
//   * `fileExists` self-roll → `ayt::io::File::exists`
//   * Temp path discovery (TEMP/TMPDIR env dance) → `ayt::io::TempFile::tempDir()`
//   * `std::ofstream` write of .sc/.varyingdef → `ayt::io::File::writeAllText`
//   * `std::ifstream` read of .bin → `ayt::io::MemoryMappedFile` (zero-copy)
//   * `::remove` cleanup → `ayt::io::File::remove`
//
// What stays:
//   * CreateProcessW / popen / ReadFile over pipe — that's OS process
//     API, not file I/O. AYIO does not (yet) expose a Process module.
//     See design.md §16.3 Future Work.

#include "AYShader/ShadercDriver.h"

#include <AYIO/File.h>
#include <AYIO/Path.h>
#include <AYIO/Env.h>

#include <atomic>
#include <chrono>
#include <memory>          // std::shared_ptr (for the global default)
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace ayt::shader
{

namespace {

// File existence check, now backed by AYIO. We previously hand-rolled
// stat() to dodge std::filesystem::exists name pollution from upstream
// AY headers; AYIO's File::exists is the canonical replacement and is
// itself a thin stat() wrapper.
inline bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    return ayt::io::File::exists(p);
}

// Spawn a child process with an arbitrary command line, capture
// combined stdout/stderr. Returns exit code (-1 if spawn failed).
//
// Mirrors the pre-Phase-3.6 Test_ShaderCompile.cpp::runShaderc()
// exactly, minus the .sc-specific bits. The {hRead, hWrite} pipe
// pattern is the standard Win32 inheritance dance; POSIX uses popen
// for brevity.
//
// Audit fix H-02 (2026-08-26): the prior version built `cmdLineW` with
// `std::wstring(cmdLine.begin(), cmdLine.end())`, which:
//
//   * constructed wchar_t values from raw bytes (UTF-8 → UTF-16 NO-OP
//     truncation — non-ASCII paths, defines, or the shaderc binary
//     name itself would have been silently corrupted);
//   * produced a std::wstring with NO trailing null, so
//     `cmdLineW.data()` is undefined behavior even when the string is
//     empty (data() since C++11 is null-terminated, but the conversion
//     was already wrong).
//
// The fix below uses MultiByteToWideChar(CP_UTF8, …) to perform the
// real UTF-8 → UTF-16 conversion into a std::vector<wchar_t> whose
// `.data()` is guaranteed null-terminated and CreateProcessW-safe.
//
// NOTE: This stays as raw OS API for now — it's process spawn, not
// file I/O. See design.md §16.3 for the future AYIO::Process module.
struct SpawnResult {
    int exitCode;
    std::string output;
    // When the spawn timed out (M-01), the child is terminated via
    // TerminateProcess and timedOut==true is set. The caller decides
    // whether to surface this as a stderr diagnostic or treat it as a
    // retryable error. The exitCode in that case is whatever the
    // kernel reports for a terminated process (often 1).
    bool timedOut = false;
};

#if defined(_WIN32)
SpawnResult spawnCapturing(const std::string& exe,
                           const std::vector<std::string>& args,
                           // M-01 timeout: 0 = wait forever (legacy
                           // default); >0 = WaitForSingleObject with
                           // this many ms, then TerminateProcess.
                           DWORD timeoutMs = 0) {
    SpawnResult r{-1, "", false};

    if (exe.empty()) {
        r.output = "spawnCapturing: executable path is empty";
        return r;
    }

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
    si.dwFlags   |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // H-02 fix: properly convert UTF-8 → UTF-16 into a vector that
    // is null-terminated (CreateProcessW's second parameter requires
    // a null-terminated wide string when the first parameter is
    // nullptr). MultiByteToWideChar(CP_UTF8, 0, …) handles the
    // non-ASCII bytes correctly; the prior `std::wstring(begin, end)`
    // form truncated everything to low bytes and dropped the
    // terminator, producing garbled command lines on Windows hosts.
    std::vector<wchar_t> cmdLineW;
    {
        const int needed = MultiByteToWideChar(
            CP_UTF8, 0, cmdLine.c_str(), -1, nullptr, 0);
        if (needed <= 0) {
            r.output = "MultiByteToWideChar size probe failed (error " +
                       std::to_string(GetLastError()) + ") for: " + cmdLine;
            CloseHandle(hRead);
            CloseHandle(hWrite);
            return r;
        }
        cmdLineW.assign(static_cast<size_t>(needed), L'\0');
        const int written = MultiByteToWideChar(
            CP_UTF8, 0, cmdLine.c_str(), -1, cmdLineW.data(), needed);
        if (written <= 0) {
            r.output = "MultiByteToWideChar conversion failed (error " +
                       std::to_string(GetLastError()) + ") for: " + cmdLine;
            CloseHandle(hRead);
            CloseHandle(hWrite);
            return r;
        }
    }

    constexpr DWORD kCreateNoWindow = 0x08000000u;
    BOOL ok = CreateProcessW(
        nullptr, cmdLineW.data(),
        nullptr, nullptr,
        TRUE, kCreateNoWindow, nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        r.output = "CreateProcess failed (error " +
                   std::to_string(GetLastError()) + ") for: " + cmdLine;
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return r;
    }
    CloseHandle(hWrite);

    // Do not perform a blocking ReadFile before waiting for the process:
    // a silent/hung child keeps its pipe open forever and would bypass the
    // timeout entirely. Poll the pipe while waiting so output-heavy shaderc
    // processes cannot block on a full pipe either.
    auto drainAvailableOutput = [&]() {
        char buf[4096];
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &available, nullptr)
                || available == 0) {
                break;
            }
            const DWORD toRead = available < sizeof(buf)
                ? available
                : static_cast<DWORD>(sizeof(buf));
            DWORD got = 0;
            if (!ReadFile(hRead, buf, toRead, &got, nullptr) || got == 0) {
                break;
            }
            r.output.append(buf, buf + got);
        }
    };

    constexpr DWORD kPollIntervalMs = 20;
    const ULONGLONG startedAt = GetTickCount64();
    DWORD waitResult = WAIT_TIMEOUT;
    for (;;) {
        drainAvailableOutput();

        DWORD waitSlice = kPollIntervalMs;
        if (timeoutMs != 0) {
            const ULONGLONG elapsed = GetTickCount64() - startedAt;
            if (elapsed >= timeoutMs) {
                waitResult = WAIT_TIMEOUT;
                break;
            }
            const DWORD remaining = timeoutMs - static_cast<DWORD>(elapsed);
            if (remaining < waitSlice) {
                waitSlice = remaining;
            }
        }

        waitResult = WaitForSingleObject(pi.hProcess, waitSlice);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED) {
            break;
        }
    }
    if (waitResult == WAIT_TIMEOUT) {
        // Hard kill the child — there's no graceful cancel signal
        // shaderc understands. The output we collected so far is
        // preserved in `r.output` for diagnostics; the bin file is
        // either absent or partial, which the caller's read-back
        // handles correctly (memory-map returns invalid for missing
        // file, "exit 0 but .bin missing" for partial).
        TerminateProcess(pi.hProcess, 1);
        // Drain the process so the kernel releases handles; this
        // also lets the exit code populate below.
        WaitForSingleObject(pi.hProcess, 5000);
        r.timedOut = true;
        r.output += "\n[shaderc] timeout after " +
                    std::to_string(timeoutMs) + "ms; terminated.";
    } else if (waitResult == WAIT_FAILED) {
        const DWORD waitError = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        r.output += "\n[shaderc] process wait failed (error " +
                    std::to_string(waitError) + ").";
    }
    drainAvailableOutput();
    CloseHandle(hRead);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    r.exitCode = static_cast<int>(exitCode);
    return r;
}
#else
SpawnResult spawnCapturing(const std::string& exe,
                           const std::vector<std::string>& args,
                           uint32_t timeoutMs = 0) {
    SpawnResult r{-1, "", false};
    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        r.output = "pipe failed";
        return r;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        r.output = "fork failed";
        return r;
    }
    if (pid == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        constexpr char kExecFailure[] = "execv failed\n";
        (void)::write(STDERR_FILENO, kExecFailure, sizeof(kExecFailure) - 1);
        ::_exit(127);
    }

    ::close(pipeFds[1]);
    const int originalFlags = ::fcntl(pipeFds[0], F_GETFL, 0);
    if (originalFlags >= 0) {
        (void)::fcntl(pipeFds[0], F_SETFL, originalFlags | O_NONBLOCK);
    }

    auto drainOutput = [&]() {
        char buf[4096];
        for (;;) {
            const ssize_t got = ::read(pipeFds[0], buf, sizeof(buf));
            if (got > 0) {
                r.output.append(buf, static_cast<size_t>(got));
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    };

    const auto startedAt = std::chrono::steady_clock::now();
    int status = 0;
    for (;;) {
        pollfd outputPoll{pipeFds[0], POLLIN, 0};
        (void)::poll(&outputPoll, 1, 20);
        drainOutput();

        const pid_t waitResult = ::waitpid(pid, &status, WNOHANG);
        if (waitResult == pid) {
            break;
        }
        if (waitResult < 0 && errno != EINTR) {
            r.output += "\n[shaderc] waitpid failed.";
            (void)::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }

        if (timeoutMs != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            if (elapsed >= timeoutMs) {
                (void)::kill(pid, SIGKILL);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                r.timedOut = true;
                r.output += "\n[shaderc] timeout after " +
                            std::to_string(timeoutMs) + "ms; terminated.";
                break;
            }
        }
    }

    drainOutput();
    ::close(pipeFds[0]);
    if (WIFEXITED(status)) {
        r.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.exitCode = 128 + WTERMSIG(status);
    }
    return r;
}
#endif

// Build a unique temp .sc path. Process-pid + monotonic counter
// sidesteps race conditions if multiple drivers / threads happen to
// compile concurrently. Caller is responsible for deleting the file
// (we RAII that in compile()).
//
// AYIO migration: we now defer to `ayt::io::TempFile::tempDir()` for
// the system temp directory instead of hand-rolling the TEMP/TMPDIR
// env-var dance. The .sc suffix is preserved so debug dumps in the
// temp directory remain human-recognizable (see design.md §16.5).
std::string uniqueScTempPath() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const unsigned pid = ::GetCurrentProcessId();
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
#endif
    const std::string dir = ayt::io::TempFile::tempDir();
    return ayt::io::path::join(
        dir,
        std::string("ayshader_") + std::to_string(pid) +
        "_" + std::to_string(n) + ".sc");
}

// Path-with-optional-extension: shaderc chooses .bin / .vert / .frag
// / .comp based on the matching --type we already pass, so we just
// slap ".bin" here. Driver cleans up after spawn.
//
// Replaces the hand-rolled `find_last_of('.')` substring replace with
// AYIO path utilities. We could go further and use MemoryMappedFile
// later, but path::stem / path::extension / path::join is enough for
// now.
std::string siblingBinPath(const std::string& scPath) {
    const std::string dir = ayt::io::path::directory(scPath);
    const std::string base = ayt::io::path::stem(ayt::io::path::filename(scPath));
    return ayt::io::path::join(dir, base + ".bin");
}

} // namespace

// M-02: validate a user-supplied shaderc executable path beyond mere
// existence. The check is intentionally conservative:
//   * path must be absolute (relative paths are ambiguous w.r.t.
//     the cwd at compile-time, and could be hijacked by a malicious
//     shim dropped into the engine's working directory);
//   * basename (without .exe on Windows) must equal "shaderc" — so a
//     user pointing at "C:\tools\foo.exe" cannot accidentally invoke
//     an unrelated binary;
//   * shell metacharacters that could let an attacker break out of
//     the quoted form in the command line (e.g. trailing "& calc.exe"
//     or `<>|^`) are rejected; we own the command line so we don't
//     need them.
//
// The validation runs before fileExists() so a malformed path fails
// fast on construction with a useful diagnostic. Returns the
// validated absolute path on success; throws std::invalid_argument on
// any rule violation.
static std::string validateShadercPath(const std::string& p) {
    if (p.empty()) {
        throw std::invalid_argument(
            "AYShadercDriver: shadercExecutable path is empty. The host engine "
            "must resolve the shaderc binary path (via env var, settings file, "
            "or startup flag) and pass it to AYShadercDriver explicitly.");
    }
#ifdef _WIN32
    // Absolute on Windows: starts with a drive letter ("X:\..." or
    // "X:/...") OR a UNC prefix ("\\server\share\..."). We don't
    // require the file to exist yet — fileExists() handles that —
    // just that the path is unambiguously rooted.
    const bool absolute =
        (p.size() >= 3 &&
         ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
         p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
        (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
    if (!absolute) {
        throw std::invalid_argument(
            "AYShadercDriver: shadercExecutable path '" + p + "' is not absolute. "
            "An absolute path is required to prevent working-directory "
            "hijack via shim binaries (audit fix M-02).");
    }
#else
    if (p.front() != '/') {
        throw std::invalid_argument(
            "AYShadercDriver: shadercExecutable path '" + p + "' is not absolute. "
            "An absolute path is required to prevent working-directory "
            "hijack via shim binaries (audit fix M-02).");
    }
#endif

    // Reject shell metacharacters that could break out of the quoted
    // command-line form (even though we quote args, defense-in-depth
    // — a future caller passing the path unquoted would otherwise be
    // vulnerable). Note: ':' is allowed (Windows drive letter), '\\'
    // and '/' are allowed (path separators). Spaces are allowed
    // because the quoting path above handles them. The dangerous set
    // is the Win32 + POSIX command-shell metacharacters.
    for (char c : p) {
        if (c == '&' || c == '|' || c == '<' || c == '>' || c == '^' ||
            c == ';' || c == '$' || c == '`' || c == '\n' || c == '\r' ||
            c == '\t') {
            throw std::invalid_argument(
                std::string("AYShadercDriver: shadercExecutable path '") + p +
                "' contains forbidden shell metacharacter '0x" +
                std::to_string(static_cast<unsigned>(c) & 0xFFu) +
                "'. Refusing to spawn (audit fix M-02).");
        }
    }

    // Basename check. We strip the directory portion and the (Windows)
    // .exe suffix; the remainder must equal "shaderc" (case-insensitive
    // on Windows, case-sensitive on POSIX — the executable is
    // always named exactly "shaderc" by upstream bgfx).
    auto findLastSep = [](const std::string& s) -> size_t {
#ifdef _WIN32
        const size_t p1 = s.find_last_of('\\');
        const size_t p2 = s.find_last_of('/');
        return p1 == std::string::npos ? p2
             : p2 == std::string::npos ? p1
             : (p1 > p2 ? p1 : p2);
#else
        return s.find_last_of('/');
#endif
    };
    const size_t sep = findLastSep(p);
    const std::string basename =
        (sep == std::string::npos) ? p : p.substr(sep + 1);
    std::string stem = basename;
#ifdef _WIN32
    // Strip a single trailing .exe (case-insensitive).
    if (stem.size() >= 4) {
        const std::string tail = stem.substr(stem.size() - 4);
        auto ieq = [](char a, char b) {
            return (a >= 'A' && a <= 'Z') ? (a + 32 == b) : a == b;
        };
        bool isExe = true;
        for (size_t i = 0; i < 4 && isExe; ++i) {
            isExe = ieq(tail[i], ".exe"[i]);
        }
        if (isExe) stem.resize(stem.size() - 4);
    }
#endif
#ifdef _WIN32
    auto ieqAll = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
            if (x != y) return false;
        }
        return true;
    };
    if (!ieqAll(stem, "shaderc")) {
#else
    if (stem != "shaderc") {
#endif
        throw std::invalid_argument(
            "AYShadercDriver: shadercExecutable basename '" + stem +
            "' does not match expected 'shaderc'. Refusing to spawn a binary "
            "that is not bgfx's shaderc (audit fix M-02).");
    }
    return p;
}

// Explicit-path constructor. The host engine resolves the shaderc
// binary path from its own config (env var, settings file, startup
// flag) and passes the absolute path here. The driver does not
// search PATH, env, or any hint file — this is intentional (sign-off
// 2026-07-01: "the user must configure the shaderc path"). Missing
// or empty path → std::invalid_argument so the caller can fail fast
// at startup rather than at first-shader-compile time.
AYShadercDriver::AYShadercDriver(const std::string& shadercExecutable) {
    _shadercPath = validateShadercPath(shadercExecutable);  // M-02: validation
    if (!fileExists(_shadercPath)) {
        throw std::invalid_argument(
            "AYShadercDriver: shaderc executable not found at '"
            + _shadercPath + "'. The host engine must resolve the shaderc "
            "binary path from its own configuration before constructing the driver.");
    }
}

// ---------------------------------------------------------------------------
// Process-wide default shaderc path (sign-off 2026-07-01)
//
// One engine = one setDefaultExecutable() call at startup. After that,
// every default-constructed AYShadercDriver in the process picks up the
// same path. Storage is a single static std::string guarded by a
// function-local initializer (Meyers singleton) — no mutex needed
// because (a) the string is read-only after the first set, (b) tests
// are single-threaded at startup, and (c) the assignment is sequenced
// before any subsequent reads thanks to the static-local
// initialization. If concurrent writes become a concern, wrap the
// static in a std::shared_ptr<const std::string> and bump a generation
// counter; we don't need that yet.
// ---------------------------------------------------------------------------
namespace {

// Lazy-initialized pointer to the process-wide default. nullptr means
// "not configured yet". We use shared_ptr<const string> so reads
// from compile threads always see a fully-constructed value (no
// torn read on the underlying std::string storage during reassign).
std::shared_ptr<const std::string>& defaultExecutable() {
    static std::shared_ptr<const std::string> ptr;
    return ptr;
}

} // namespace

void AYShadercDriver::setDefaultExecutable(const std::string& path) {
    defaultExecutable() = std::make_shared<const std::string>(path);
}

void AYShadercDriver::clearDefaultExecutable() {
    defaultExecutable().reset();
}

bool AYShadercDriver::hasDefaultExecutable() {
    const auto p = defaultExecutable();
    return p && !p->empty();
}

// Default constructor: looks up the process-wide default. Throws
// std::runtime_error if no default has been set (the user forgot
// the startup call) or std::invalid_argument if the configured
// path doesn't point to an existing file.
// (Destructor moved to header to avoid LNK4006 — see header comment.)

AYShadercDriver::AYShadercDriver() {
    auto p = defaultExecutable();
    if (!p) {
        throw std::runtime_error(
            "AYShadercDriver: no default executable configured. Call "
            "AYShadercDriver::setDefaultExecutable(<path>) at engine "
            "startup before constructing the driver.");
    }
    if (p->empty()) {
        throw std::runtime_error(
            "AYShadercDriver: default executable is an empty string. "
            "Re-call setDefaultExecutable with a valid path.");
    }
    // M-02: validate before existence so a configured malformed path
    // (relative, basename mismatch, metacharacters) fails with a
    // useful diagnostic instead of just "not found".
    _shadercPath = validateShadercPath(*p);
    if (!fileExists(_shadercPath)) {
        throw std::invalid_argument(
            "AYShadercDriver: configured default shaderc executable not "
            "found at '" + _shadercPath + "'. Verify the path and call "
            "setDefaultExecutable again.");
    }
}

ShaderCompileResult AYShadercDriver::compile(const ShaderCompileRequest& req) {
    ShaderCompileResult result;

    if (_shadercPath.empty()) {
        result.stderrText = "shaderc path is empty; refusing to spawn";
        return result;
    }
    std::string shadercPath = _shadercPath;

    // 1) Stage the in-memory .sc to a temp file.
    std::string scPath = uniqueScTempPath();
    std::string binPath = siblingBinPath(scPath);

    if (!ayt::io::File::writeAllText(scPath, req.scSource)) {
        result.stderrText = "cannot write temp .sc: " + scPath;
        return result;
    }

    // RAII guard: delete temp files on every exit path. We capture
    // both paths by value so the lambda outlives local variables.
    auto cleanup = [&]() {
        ayt::io::File::remove(scPath);
        ayt::io::File::remove(binPath);
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
        if (!ayt::io::File::writeAllText(vdPath, req.varyingdefSource)) {
            cleanup();
            result.stderrText = "cannot write temp varyingdef: " + vdPath;
            return result;
        }
        args.push_back("--varyingdef"); args.push_back(vdPath);
        // The varyingdef sibling is also a temp file; it should not
        // outlive the call. Track it for cleanup.
        auto cleanupVd = [&]() {
            ayt::io::File::remove(vdPath);
        };
        // Run the actual spawn and read-back through a scope that
        // deletes vdPath on the way out regardless of outcome.
        // M-01: thread req.timeoutMs through (0 = INFINITE for legacy
        // callers, >0 = bounded wait + TerminateProcess on timeout).
        SpawnResult sr = spawnCapturing(shadercPath, args, req.timeoutMs);
        if (sr.exitCode == 0 && !sr.timedOut) {
            ayt::io::MemoryMappedFile mm(binPath);
            if (mm.isValid()) {
                const uint8_t* p = static_cast<const uint8_t*>(mm.data());
                result.bytes.assign(p, p + mm.size());
                result.ok = !result.bytes.empty();
            } else {
                result.stderrText = "shaderc exit 0 but .bin missing: " + binPath;
            }
        } else {
            result.stderrText = "shaderc exit " + std::to_string(sr.exitCode) +
                                ": " + sr.output + "\n(request: " + req.outputName + ")";
        }
        cleanupVd();
        cleanup();
        return result;
    }

    // 3) Spawn. M-01: thread req.timeoutMs through (0 = INFINITE legacy,
    // >0 = bounded wait + TerminateProcess on timeout).
    SpawnResult sr = spawnCapturing(shadercPath, args, req.timeoutMs);

    // 4) Read .bin back into memory via MemoryMappedFile (zero-copy
    //    versus the pre-migration std::ifstream + istreambuf_iterator).
    // M-01: a timedOut result counts as a failure even when
    // GetExitCodeProcess reports 0 from a terminated child — there is
    // no valid .bin in that case, so ok stays false and the timeout
    // text from spawnCapturing is what the frontend sees.
    if (sr.exitCode == 0 && !sr.timedOut) {
        ayt::io::MemoryMappedFile mm(binPath);
        if (mm.isValid()) {
            const uint8_t* p = static_cast<const uint8_t*>(mm.data());
            result.bytes.assign(p, p + mm.size());
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
