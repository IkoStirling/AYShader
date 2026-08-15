#pragma once
// detail/AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/ShaderHandleEncoding.h — pool-scoped opaque ids (Phase 4-O)

#include <cstdint>

namespace ayt::shader::detail
{

inline uint64_t makeShaderHandle(uint32_t poolSerial, uint32_t localId)
{
    return (static_cast<uint64_t>(poolSerial) << 32) | static_cast<uint64_t>(localId);
}

inline uint32_t shaderHandlePoolSerial(uint64_t handle)
{
    return static_cast<uint32_t>(handle >> 32);
}

inline uint32_t shaderHandleLocalId(uint64_t handle)
{
    return static_cast<uint32_t>(handle & 0xFFFFFFFFu);
}

} // namespace ayt::shader::detail
