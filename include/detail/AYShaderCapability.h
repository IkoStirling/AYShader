#pragma once
// detail/AYShaderCapability.h — capability-based backend selection (Phase 4-L)

#include <cstdint>

namespace ayt::shader
{

enum class ShaderCapability : uint32_t {
    None            = 0,
    VertexFragment  = 1u << 0,
    Compute         = 1u << 1,
    Ubo             = 1u << 2,
    Ssbo            = 1u << 3,
};

inline ShaderCapability operator|(ShaderCapability a, ShaderCapability b)
{
    return static_cast<ShaderCapability>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ShaderCapability operator&(ShaderCapability a, ShaderCapability b)
{
    return static_cast<ShaderCapability>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasCapability(ShaderCapability set, ShaderCapability flag)
{
    return (set & flag) == flag;
}

} // namespace ayt::shader
