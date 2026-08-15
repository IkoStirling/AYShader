// Test_Std140Layout.cpp �?Phase 4-D std140 layout calculator

#include "AYShader/detail/Std140Layout.h"
#include "AYTest.h"

using namespace ayt::shader::detail;

TEST_SUITE(Std140LayoutTests)

TEST_CASE(vec3_then_float_packs_with_padding)
{
    Std140Layout layout;
    std::string error;
    const bool ok = computeStd140Layout(
        {"position", "fov"},
        {"vec3", "float"},
        layout,
        &error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(layout.sizeBytes == 16);
    CHECK(layout.members.size() == 2);
    CHECK(layout.members[0].name == "position");
    CHECK(layout.members[0].offsetBytes == 0);
    CHECK(layout.members[0].sizeBytes == 12);
    CHECK(layout.members[1].name == "fov");
    CHECK(layout.members[1].offsetBytes == 12);
    CHECK(layout.members[1].sizeBytes == 4);
}

TEST_CASE(mat4_is_64_bytes)
{
    Std140Layout layout;
    CHECK(computeStd140Layout({"mvp"}, {"mat4"}, layout, nullptr));
    CHECK(layout.sizeBytes == 64);
    CHECK(layout.members[0].offsetBytes == 0);
    CHECK(layout.members[0].sizeBytes == 64);
}

TEST_CASE(two_vec3_fields_align_to_32_bytes)
{
    Std140Layout layout;
    CHECK(computeStd140Layout({"a", "b"}, {"vec3", "vec3"}, layout, nullptr));
    CHECK(layout.members[0].offsetBytes == 0);
    CHECK(layout.members[1].offsetBytes == 16);
    CHECK(layout.sizeBytes == 32);
}

TEST_CASE(unsupported_type_fails)
{
    Std140Layout layout;
    std::string error;
    CHECK_FALSE(computeStd140Layout({"x"}, {"double"}, layout, &error));
    CHECK(!error.empty());
}

// ===== Phase 1 RD-03: UBO array layout =====

TEST_CASE(mat4_array_of_128_is_8192_bytes) {
    // Phase 1 RD-03: a `mat4 bones[128]` UBO field. Per-element size
    // is 64 B (mat4), per-element alignment is 16, stride = 64
    // (already aligned), total = 64 * 128 = 8192 B. The block rounds
    // up to 16-byte alignment which is already 16.
    Std140Layout layout;
    const bool ok = computeStd140Layout(
        {"bones"},
        {"mat4"},
        layout,
        nullptr,
        {128});
    CHECK(ok);
    CHECK(layout.sizeBytes == 128u * 64u);
    CHECK(layout.members.size() == 1);
    CHECK(layout.members[0].name == "bones");
    CHECK(layout.members[0].arrayLength == 128u);
    CHECK(layout.members[0].strideBytes == 64u);
    CHECK(layout.members[0].sizeBytes == 128u * 64u);
}

TEST_CASE(array_layout_rounds_up_to_vec4) {
    // Phase 1 RD-03: a `vec3 arr[3]` field. vec3 is 12 B but std140
    // rounds the stride up to 16 (vec4 alignment) for array stride.
    // Total = 3 * 16 = 48 B; block size rounds up to 16 → 48.
    Std140Layout layout;
    const bool ok = computeStd140Layout(
        {"arr"},
        {"vec3"},
        layout,
        nullptr,
        {3});
    CHECK(ok);
    CHECK(layout.members[0].arrayLength == 3u);
    CHECK(layout.members[0].strideBytes == 16u);
    CHECK(layout.members[0].sizeBytes == 3u * 16u);
    CHECK(layout.sizeBytes == 48u);
}

TEST_CASE(default_array_length_is_one) {
    // Backward-compat path: when the arrayLengths vector is empty /
    // absent, the existing single-element behavior must hold.
    Std140Layout layout;
    const bool ok = computeStd140Layout(
        {"mvp"},
        {"mat4"},
        layout,
        nullptr);
    CHECK(ok);
    CHECK(layout.members[0].arrayLength == 1u);
    CHECK(layout.members[0].strideBytes == 64u);
    CHECK(layout.members[0].sizeBytes == 64u);
    CHECK(layout.sizeBytes == 64u);
}

TEST_SUITE_END
