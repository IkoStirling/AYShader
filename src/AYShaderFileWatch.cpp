// AYShaderFileWatch.cpp — source file helpers (Phase 4-J)

#include "detail/AYShaderFileWatch.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#endif

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
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) {
        return std::nullopt;
    }
    ULARGE_INTEGER ull;
    ull.LowPart = info.ftLastWriteTime.dwLowDateTime;
    ull.HighPart = info.ftLastWriteTime.dwHighDateTime;
    return static_cast<int64_t>(ull.QuadPart / 10000);
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return static_cast<int64_t>(st.st_mtime) * 1000
         + static_cast<int64_t>(st.st_mtim.tv_nsec / 1000000);
#endif
}

bool readTextFile(const std::string& path, std::string& out, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "failed to open source file: " + path;
        }
        return false;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        if (error != nullptr) {
            *error = "failed to read source file size: " + path;
        }
        return false;
    }

    out.resize(static_cast<size_t>(size));
    if (size == 0) {
        return true;
    }

    in.seekg(0, std::ios::beg);
    in.read(out.data(), size);
    if (!in) {
        if (error != nullptr) {
            *error = "failed to read source file: " + path;
        }
        return false;
    }
    return true;
}

int64_t steadyClockMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ayt::shader::detail
