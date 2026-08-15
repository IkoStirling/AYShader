// Test_HandleABI.cpp �?Phase 4-O opaque handle ABI

#include "AYShader/ShaderResource.h"
#include "AYTest.h"

#include <unordered_map>

using namespace ayt::shader;

TEST_SUITE(HandleABITests)

TEST_CASE(shader_resource_is_eight_bytes)
{
    CHECK(sizeof(ShaderResource) == sizeof(uint64_t));
}

TEST_CASE(shader_resource_copy_preserves_id)
{
    ShaderResource a(42);
    ShaderResource b = a;
    CHECK(a.id() == b.id());
    CHECK(a == b);
}

TEST_CASE(shader_resource_usable_as_map_key)
{
    std::unordered_map<ShaderResource, int> table;
    ShaderResource key(7);
    table[key] = 123;
    CHECK(table[key] == 123);
}

TEST_SUITE_END
