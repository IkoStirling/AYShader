// AYStd140Layout.cpp - OpenGL std140 layout rules (Phase 4-D)

#include "detail/AYStd140Layout.h"

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

bool baseAlignmentAndSize(const std::string& type, size_t& alignment, size_t& size)
{
    if (type == "float" || type == "int" || type == "uint" || type == "bool") {
        alignment = 4;
        size = 4;
        return true;
    }
    if (type == "vec2" || type == "ivec2" || type == "uvec2") {
        alignment = 8;
        size = 8;
        return true;
    }
    if (type == "vec3" || type == "ivec3" || type == "uvec3") {
        alignment = 16;
        size = 12;
        return true;
    }
    if (type == "vec4" || type == "ivec4" || type == "uvec4") {
        alignment = 16;
        size = 16;
        return true;
    }
    if (type == "mat2") {
        alignment = 8;
        size = 16;
        return true;
    }
    if (type == "mat3") {
        alignment = 16;
        size = 48;
        return true;
    }
    if (type == "mat4") {
        alignment = 16;
        size = 64;
        return true;
    }
    return false;
}

} // namespace

bool computeStd140Layout(const std::vector<std::string>& names,
                         const std::vector<std::string>& types,
                         Std140Layout& out,
                         std::string* error)
{
    out = Std140Layout{};
    if (names.size() != types.size()) {
        if (error) {
            *error = "std140 layout: field name/type count mismatch";
        }
        return false;
    }

    size_t offset = 0;
    out.members.reserve(names.size());

    for (size_t i = 0; i < names.size(); ++i) {
        size_t alignment = 0;
        size_t size = 0;
        if (!baseAlignmentAndSize(types[i], alignment, size)) {
            if (error) {
                *error = "std140 layout: unsupported field type '" + types[i] + "'";
            }
            return false;
        }

        offset = roundUp(offset, alignment);

        Std140Member member;
        member.name = names[i];
        member.type = types[i];
        member.offsetBytes = offset;
        member.sizeBytes = size;
        out.members.push_back(std::move(member));

        offset += size;
    }

    out.sizeBytes = roundUp(offset, 16);
    return true;
}

} // namespace ayt::shader::detail
