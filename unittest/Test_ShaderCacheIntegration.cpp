// Test_ShaderCacheIntegration.cpp �?Phase 4-K (frontend header contract)
//
// This TU intentionally does NOT #include <bgfx/bgfx.h>.
// If any frontend header pulls bgfx into this compilation unit, the
// build breaks or we lose the Phase 4-E isolation guarantee.

#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResource.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYTest.h"

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

TEST_SUITE(ShaderCacheIntegrationTests)

TEST_CASE(frontend_pool_api_without_bgfx_header)
{
    ShaderResourcePool pool;
    pool.setPlatform("windows");
    pool.setGLSLProfile("430");
    pool.setShadercExecutable("nonexistent/shaderc.exe");

    ShaderResource res = pool.compile("material X { vertex { return vec4(0); } fragment { return vec4(1); } }");
    CHECK_FALSE(res.isValid());

    pool.shutdown();
}

TEST_CASE(frontend_compiler_delegates_to_pool)
{
    ShaderResourcePool pool;

    Compiler compiler;
    ShaderResource res = compiler.compileToShaderResource("not valid phoskia", pool);
    CHECK_FALSE(res.isValid());

    pool.shutdown();
}

TEST_CASE(frontend_binding_queries_are_opaque)
{
    ShaderResource res;
    CHECK_INT_EQ(res.getUniformBinding("anything"), InvalidBinding);
    CHECK_INT_EQ(res.getTextureBinding("tex"), InvalidBinding);
    CHECK_INT_EQ(res.getUniformBlockBinding("Camera"), InvalidBinding);
    CHECK_INT_EQ(res.getStorageBufferBinding("data"), InvalidBinding);
}

TEST_SUITE_END
