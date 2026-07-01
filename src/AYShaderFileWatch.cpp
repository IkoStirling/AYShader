// AYShaderFileWatch.cpp — source file helpers (Phase 4-J)
//
// File I/O migrated to AYFoundation/AYIO (ayt::io). The hot-reload semantics
// (mtime in milliseconds, file existence checks, source text reads) are
// preserved; only the underlying open/stat/ifstream plumbing moved.
//
// Note on mtime precision: pre-migration this returned int64_t milliseconds
// derived from POSIX st_mtim.tv_nsec / 1e6 (sub-millisecond jitter present).
// The AYIO `File::lastModifiedTime` API returns whole Unix seconds, so we
// multiply by 1000. The loss of sub-second precision is acceptable for
// hot-reload — file modifications are seconds-spaced events. See
// design.md §16.4 for the full reasoning.

#include "detail/AYShaderFileWatch.h"

#include <AYFile.h>

#include <algorithm>
#include <chrono>

namespace ayt::shader::detail
{

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
    // AYIO returns Unix timestamp in seconds; convert to milliseconds.
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