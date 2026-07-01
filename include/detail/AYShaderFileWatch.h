#pragma once
// detail/AYShaderFileWatch.h — source file helpers (Phase 4-J)

#include <cstdint>
#include <optional>
#include <string>

namespace ayt::shader::detail
{

std::string normalizeSourcePath(const std::string& path);
std::optional<int64_t> fileMtimeMs(const std::string& path);
bool readTextFile(const std::string& path, std::string& out, std::string* error = nullptr);
int64_t steadyClockMs();

} // namespace ayt::shader::detail
