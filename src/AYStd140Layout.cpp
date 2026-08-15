// AYStd140Layout.cpp - OpenGL std140 layout rules (Phase 4-D)
//
// Phase 1 RD-04: arrays of fixed size (`mat4 bones[128];`) are laid out
// per std140 rules — alignment is the element alignment (mat4 = 16),
// total size is N * stride, where stride = roundUp(elementSize, alignment).
// For mat4 that's 64 (no padding needed); for mat3 we'd pad each element
// to 64 bytes (48 + 16 tail padding) so subsequent elements stay aligned.

#include "AYShader/detail/Std140Layout.h"

namespace ayt::shader::detail
{

namespace {

size_t roundUp(size_t value, size_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    const size_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

} // namespace

bool std140ElementAlignmentAndSize(const std::string& type,
                                   size_t& elementAlignment,
                                   size_t& elementSize)
{
    // Phase 1 RD-04: accept GLSL canonical form `mat4x4` (the IR
    // MatrixType::toString() returns it) as well as the legacy
    // single-number form `mat4`. Same for mat2x2/mat3x3. Std140
    // alignment is the same regardless.
    if (type == "mat4x4" || type == "mat4") {
        elementAlignment = 16;
        elementSize = 64;
        return true;
    }
    if (type == "mat3x3" || type == "mat3") {
        elementAlignment = 16;
        elementSize = 48;
        return true;
    }
    if (type == "mat2x2" || type == "mat2") {
        elementAlignment = 8;
        elementSize = 16;
        return true;
    }
    if (type == "float" || type == "int" || type == "uint" || type == "bool") {
        elementAlignment = 4;
        elementSize = 4;
        return true;
    }
    if (type == "vec2" || type == "ivec2" || type == "uvec2") {
        elementAlignment = 8;
        elementSize = 8;
        return true;
    }
    if (type == "vec3" || type == "ivec3" || type == "uvec3") {
        elementAlignment = 16;
        elementSize = 12;
        return true;
    }
    if (type == "vec4" || type == "ivec4" || type == "uvec4") {
        elementAlignment = 16;
        elementSize = 16;
        return true;
    }
    if (type == "mat2") {
        elementAlignment = 8;
        elementSize = 16;
        return true;
    }
    if (type == "mat3") {
        elementAlignment = 16;
        elementSize = 48;
        return true;
    }
    if (type == "mat4") {
        elementAlignment = 16;
        elementSize = 64;
        return true;
    }
    return false;
}

namespace {

bool baseAlignmentAndSize(const std::string& type, size_t& alignment, size_t& size)
{
    return std140ElementAlignmentAndSize(type, alignment, size);
}

} // namespace

bool computeStd140Layout(const std::vector<std::string>& names,
                         const std::vector<std::string>& types,
                         Std140Layout& out,
                         std::string* error,
                         const std::vector<size_t>& arrayLengths)
{
    out = Std140Layout{};
    if (names.size() != types.size()) {
        if (error) {
            *error = "std140 layout: field name/type count mismatch";
        }
        return false;
    }
    if (!arrayLengths.empty() && arrayLengths.size() != names.size()) {
        if (error) {
            *error = "std140 layout: arrayLengths count mismatch";
        }
        return false;
    }

    size_t offset = 0;
    out.members.reserve(names.size());

    for (size_t i = 0; i < names.size(); ++i) {
        size_t elementAlignment = 0;
        size_t elementSize = 0;
        if (!baseAlignmentAndSize(types[i], elementAlignment, elementSize)) {
            if (error) {
                *error = "std140 layout: unsupported field type '" + types[i] + "'";
            }
            return false;
        }

        // Phase 1 RD-04: optional array suffix. arrayLengths default to 1
        // (non-array) when the caller didn't supply them.
        const size_t arrayLength = arrayLengths.empty() ? 1 : arrayLengths[i];
        if (arrayLength == 0) {
            if (error) {
                *error = "std140 layout: arrayLength must be >= 1 (got 0 for '" + names[i] + "')";
            }
            return false;
        }

        // For non-array fields the stored size equals elementSize — a
        // vec3 takes 12 bytes even though its alignment is 16 (the
        // *next* field picks up the trailing padding). For arrays we
        // need to record N * stride so the GLSL emit and uniform-block
        // upload produce the right byte count.
        const size_t alignment = elementAlignment;
        const size_t stride = roundUp(elementSize, alignment);
        const size_t totalSize = (arrayLength == 1)
            ? elementSize
            : stride * arrayLength;

        offset = roundUp(offset, alignment);

        Std140Member member;
        member.name = names[i];
        member.type = types[i];
        member.offsetBytes = offset;
        member.sizeBytes = totalSize;
        member.arrayLength = arrayLength;
        member.strideBytes = stride;
        out.members.push_back(std::move(member));

        offset += totalSize;
    }

    out.sizeBytes = roundUp(offset, 16);
    return true;
}

} // namespace ayt::shader::detail
