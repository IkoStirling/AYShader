// Test_CapabilityDispatch.cpp â€?Phase 4-L capability selection

#include "AYShaderResourcePool.h"
#include "AYTest.h"
#include "detail/AYShaderCapability.h"

using namespace ayt::shader;

TEST_SUITE(CapabilityDispatchTests)

TEST_CASE(require_sets_capability_mask)
{
    ShaderResourcePool pool;
    pool.require(ShaderCapability::VertexFragment | ShaderCapability::Ubo);
    pool.shutdown();
}

TEST_CASE(has_capability_helper)
{
    const ShaderCapability caps =
        ShaderCapability::VertexFragment | ShaderCapability::Compute;
    CHECK(hasCapability(caps, ShaderCapability::VertexFragment));
    CHECK_FALSE(hasCapability(caps, ShaderCapability::Ubo));
}

TEST_SUITE_END
