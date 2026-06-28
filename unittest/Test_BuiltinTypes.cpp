// ============================================================
// AYShader BuiltinTypes Unit Tests (Phase 2 Step 5)
// ============================================================
//
// Exercises AYBuiltinTypes::isBuiltinType / all() / expectedList —
// the lookup table that the parser / semantic analyzer consult after
// the Step 5 token-demotion refactor to decide whether an Identifier
// token carries a builtin-type name.

#include "AYBuiltinTypes.h"
#include "AYTest.h"

#include <string_view>

using namespace ayt::shader::phoskia;

TEST_SUITE(BuiltinTypesTests)

// ===== Recognized names =====

TEST_CASE(builtin_float_recognized) {
    CHECK(AYBuiltinTypes::isBuiltinType("float"));
}

TEST_CASE(builtin_vec_recognized) {
    CHECK(AYBuiltinTypes::isBuiltinType("vec2"));
    CHECK(AYBuiltinTypes::isBuiltinType("vec3"));
    CHECK(AYBuiltinTypes::isBuiltinType("vec4"));
}

TEST_CASE(builtin_int_and_ivec_recognized) {
    CHECK(AYBuiltinTypes::isBuiltinType("int"));
    CHECK(AYBuiltinTypes::isBuiltinType("ivec2"));
    CHECK(AYBuiltinTypes::isBuiltinType("ivec3"));
    CHECK(AYBuiltinTypes::isBuiltinType("ivec4"));
}

TEST_CASE(builtin_mat_recognized) {
    CHECK(AYBuiltinTypes::isBuiltinType("mat2"));
    CHECK(AYBuiltinTypes::isBuiltinType("mat3"));
    CHECK(AYBuiltinTypes::isBuiltinType("mat4"));
}

TEST_CASE(builtin_quat_and_bool_recognized) {
    CHECK(AYBuiltinTypes::isBuiltinType("quat"));
    CHECK(AYBuiltinTypes::isBuiltinType("bool"));
}

// ===== Rejected names =====

TEST_CASE(non_builtin_user_name_rejected) {
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("hello"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("cameraPos"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("myUniform"));
}

TEST_CASE(non_builtin_reserved_keyword_rejected) {
    // Phoskia semantic keywords look like type names but are NOT
    // builtin types — they're for in/out parameter declarations only.
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("position"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("normal"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("color"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("texcoord"));
}

TEST_CASE(non_builtin_statement_keyword_rejected) {
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("material"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("vertex"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("fragment"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("let"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("return"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("if"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("for"));
}

TEST_CASE(non_builtin_empty_and_garbage_rejected) {
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType(""));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("Float"));  // case-sensitive
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("vec33"));   // typo
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("vec"));    // missing dimension
}

// ===== string_view API =====

TEST_CASE(string_view_overload_works) {
    std::string_view sv = "vec3";
    CHECK(AYBuiltinTypes::isBuiltinType(sv));
    std::string_view bad = "hello";
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType(bad));
}

// ===== all() / expectedList() =====

TEST_CASE(all_returns_full_set) {
    const auto& names = AYBuiltinTypes::all();
    CHECK(names.size() == 13);
    CHECK(names.count("float") == 1);
    CHECK(names.count("vec3") == 1);
    CHECK(names.count("mat4") == 1);
    CHECK(names.count("bool") == 1);
    CHECK(names.count("quat") == 1);
}

TEST_CASE(expected_list_is_non_empty_and_mentions_common_types) {
    auto list = AYBuiltinTypes::expectedList();
    CHECK_FALSE(list.empty());
    CHECK(list.find("float") != std::string::npos);
    CHECK(list.find("vec3") != std::string::npos);
    CHECK(list.find("mat4") != std::string::npos);
    CHECK(list.find("bool") != std::string::npos);
}

TEST_SUITE_END