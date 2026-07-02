// AYShaderFileWatch.cpp — source file helpers (Phase 4-J)
//
// File I/O migrated to AYFoundation/AYIO (ayt::io). The hot-reload semantics
// (mtime in milliseconds, file existence checks, source text reads) are
// preserved; only the underlying open/stat/ifstream plumbing moved.
//
// Note on mtime precision: hot-reload compares millisecond timestamps.
// Windows uses GetFileAttributesEx (100-ns FILETIME); POSIX uses st_mtim /
// st_mtimespec when available. AYIO whole-second fallback is used only when
// platform stat APIs are unavailable.

#include "detail/AYShaderFileWatch.h"

#include <AYFile.h>

#include <algorithm>
#include <chrono>
#include <sys/stat.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#endif

namespace ayt::shader::detail
{

namespace {

int64_t filetimeToUnixMs(const FILETIME& ft)
{
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    constexpr uint64_t kWindowsEpochToUnix100Ns = 116444736000000000ULL;
    if (ull.QuadPart < kWindowsEpochToUnix100Ns) {
        return 0;
    }
    return static_cast<int64_t>((ull.QuadPart - kWindowsEpochToUnix100Ns) / 10000ULL);
}

} // namespace

std::string normalizeSourcePath(const std::string& path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

std::optional<int64_t> fileMtimeMs(const std::string& path)
{
    if (!ayt::io::File::exists(path)) {
        return std::nullopt;
    }

#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs) != 0) {
        const int64_t ms = filetimeToUnixMs(attrs.ftLastWriteTime);
        if (ms > 0) {
            return ms;
        }
    }
#endif

    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) {
#if defined(_WIN32)
        return static_cast<int64_t>(st.st_mtime) * 1000;
#elif defined(__APPLE__)
        return static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000
             + static_cast<int64_t>(st.st_mtimespec.tv_nsec / 1000000L);
#else
        return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000
             + static_cast<int64_t>(st.st_mtim.tv_nsec / 1000000L);
#endif
    }

    // Fallback: whole-second timestamp from AYIO.
    const uint64_t sec = ayt::io::File::lastModifiedTime(path);
    if (sec == 0) {
        return std::nullopt;
    }
    return static_cast<int64_t>(sec) * 1000;
}

bool readTextFile(const std::string& path, std::string& out, std::string* error)
{
    if (!ayt::io::File::exists(path)) {
        if (error != nullptr) {
            *error = "failed to open source file: " + path;
        }
        return false;
    }
    // readAllText returns "" on open failure or empty file. We disambiguate
    // "file does not exist" (handled above) from "empty file" (success, out
    // stays empty). After this point, an empty return == open failure post-
    // existence-check (e.g. permission denied mid-call) — extremely rare.
    const std::string contents = ayt::io::File::readAllText(path);
    // We can't perfectly distinguish "empty file" from "readAllText failed
    // after the exists() probe". Pre-migration's std::ifstream operator!
    // distinguished them via `if (!in)`. We mirror that by re-checking the
    // size: if exists() says yes, readAllText of a non-empty file must
    // produce bytes. readAllText returning "" on a >0-byte file is a
    // genuine failure.
    if (contents.empty()) {
        const auto attrs = ayt::io::File::queryAttributes(path);
        if (attrs.size > 0) {
            if (error != nullptr) {
                *error = "failed to read source file: " + path;
            }
            return false;
        }
    }
    out = std::move(contents);
    return true;
}

int64_t steadyClockMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ayt::shader::detail