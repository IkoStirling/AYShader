#pragma once
// detail/AYStd140Layout.h - std140 UBO layout (internal, Phase 4-D)

#include <cstddef>
#include <string>
#include <vector>

namespace ayt::shader::detail
{

struct Std140Member {
    std::string name;
    std::string type;
    size_t      offsetBytes = 0;
    size_t      sizeBytes = 0;
};

struct Std140Layout {
    size_t                    sizeBytes = 0;
    std::vector<Std140Member> members;
};

bool computeStd140Layout(const std::vector<std::string>& names,
                         const std::vector<std::string>& types,
                         Std140Layout& out,
                         std::string* error = nullptr);

} // namespace ayt::shader::detail
