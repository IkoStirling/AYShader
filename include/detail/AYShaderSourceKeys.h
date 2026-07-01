#pragma once
// detail/AYShaderSourceKeys.h — neutral stage keys (Phase 4-P)

#include <cstddef>
#include <string>

namespace ayt::shader::detail
{

inline std::string vertexStageKey(size_t index)
{
    return "vertex_stage_" + std::to_string(index);
}

inline std::string fragmentStageKey(size_t index)
{
    return "fragment_stage_" + std::to_string(index);
}

inline std::string computeStageKey(size_t index)
{
    return "compute_stage_" + std::to_string(index);
}

inline constexpr const char* kVaryingDefinitionsKey = "varying_definitions";

} // namespace ayt::shader::detail
