// Test_Std140Layout.cpp — Phase 4-D std140 layout calculator

#include "detail/AYStd140Layout.h"
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

TEST_SUITE_END
