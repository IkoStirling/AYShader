// Test_CacheStats.cpp â€?Phase 4-Q two-tier cache stats

#include "AYPhoskia.h"
#include "AYShaderResourcePool.h"
#include "AYShadercDriver.h"
#include "AYTest.h"

#include <bgfx/bgfx.h>
#include <sys/stat.h>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#endif
#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
#endif

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

const std::string kSrc = R"(
material Unlit {
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0);
    vertex { return vec4(0.0, 0.0, 0.0, 1.0); }
    fragment { return baseColor; }
}
)";

bool envReady()
{
    struct stat st;
    return ::stat(AY_SHADER_SHADERC_HINT, &st) == 0
        && ::stat(AY_SHADER_BGFX_COMMON_HINT, &st) == 0;
}

} // namespace

TEST_SUITE(CacheStatsTests)

TEST_CASE(second_compile_records_source_and_binary_hits)
{
    if (!envReady()) {
        return;
    }

    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init)) {
        return;
    }

    AYShadercDriver::setDefaultExecutable(AY_SHADER_SHADERC_HINT);

    ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    if (AY_SHADER_BGFX_COMMON_HINT[0]) {
        pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT});
    }

    (void)pool.compile(kSrc);
    CacheStats afterFirst = pool.cacheStats();
    CHECK(afterFirst.sourceMisses >= 1);
    CHECK(afterFirst.binaryMisses >= 1);

    (void)pool.compile(kSrc);
    CacheStats afterSecond = pool.cacheStats();
    CHECK(afterSecond.sourceHits >= 1);
    CHECK(afterSecond.binaryHits >= 1);

    pool.shutdown();
    bgfx::shutdown();
}

TEST_SUITE_END
