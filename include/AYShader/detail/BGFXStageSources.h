#pragma once
// detail/AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/BGFXStageSources.h — backend-internal stage text (Phase 4-H)

#include <string>

namespace ayt::shader::detail
{

struct BGFXMaterialStages {
    std::string vertex;
    std::string fragment;
    std::string varyingDefinitions;
};

struct BGFXComputeStage {
    std::string compute;
};

} // namespace ayt::shader::detail
