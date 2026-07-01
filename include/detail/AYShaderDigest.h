#pragma once
// detail/AYShaderDigest.h — cache key digest (Phase 4-I)

#include <array>
#include <cstdint>
#include <string>

namespace ayt::shader::detail
{

std::array<uint8_t, 32> sha256(const std::string& data);
std::string sha256Hex(const std::string& data);

} // namespace ayt::shader::detail
