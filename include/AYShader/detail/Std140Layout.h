#pragma once
// detail/AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/Std140Layout.h - std140 UBO layout (internal, Phase 4-D)

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
    // Phase 1 RD-04: array length (1 = non-array). For an array of
    // `mat4 bones[N]`, the member's sizeBytes already covers N*elementSize
    // and strideBytes equals element alignment / element size for indexing.
    size_t      arrayLength = 1;
    size_t      strideBytes = 0;
};

struct Std140Layout {
    size_t                    sizeBytes = 0;
    std::vector<Std140Member> members;
};

// Per-element std140 entry. Returns false if `type` isn't recognized.
// `elementAlignment` is the per-element alignment (e.g. 16 for mat4);
// `elementSize` is the per-element size (e.g. 64 for mat4).
bool std140ElementAlignmentAndSize(const std::string& type,
                                   size_t& elementAlignment,
                                   size_t& elementSize);

// Phase 1 RD-04: compute std140 layout for a uniform block.
// `arrayLengths[i] == 1` (or absent) treats field i as a non-array.
// `arrayLengths[i] > 1` makes it a fixed-size array; per std140 rules
// the alignment is the element alignment (mat4 = 16) and total size
// is `arrayLength * stride`, where stride = element size round-up to
// the element's alignment. For mat4 (alignment 16, size 64) the stride
// is 64. Total block size rounds up to 16 bytes.
bool computeStd140Layout(const std::vector<std::string>& names,
                         const std::vector<std::string>& types,
                         Std140Layout& out,
                         std::string* error = nullptr,
                         const std::vector<size_t>& arrayLengths = {});

} // namespace ayt::shader::detail
