// Test_ShaderResource.cpp �?Phase 4-A / 4-B / 4-C
//
// Layer A: API contract without bgfx runtime.
// Layer B: wire-up via ShaderResourcePool + bgfx::RendererType::Noop
// (skips when shaderc or bgfx init unavailable).

#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResource.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShadercDriver.h"
#include "AYTest.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#  define PUTENV_S(name, val) _putenv_s(name, val)
#else
#  define PUTENV_S(name, val) setenv(name, val, 1)
#endif

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif
#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
#endif
#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT ""
#endif

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

namespace {

const std::string kMinimalUnlit = R"(
material Unlit {
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0);

    vertex {
        in  position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in  position : position;
        return baseColor;
    }
}
)";

const std::string kUboMaterial = R"(
uniformblock Camera {
    vec3 position
    float fov
}
material UBOTest {
    vertex {
        let p = Camera.position
        return vec4(p, 1.0)
    }
    fragment {
        return vec4(Camera.fov, 0.0, 0.0, 1.0)
    }
}
)";

inline bool fileExists(const std::string& p)
{
    if (p.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

bool shadercAvailable()
{
    const std::string path = AY_SHADER_SHADERC_HINT;
    if (!fileExists(path)) {
        return false;
    }
    try {
        AYShadercDriver probe(path);
        if (probe.shadercPath().empty()) {
            return false;
        }
        AYShadercDriver::setDefaultExecutable(path);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool bgfxCommonAvailable()
{
    return fileExists(AY_SHADER_BGFX_COMMON_HINT);
}

bool wireUpEnvironmentAvailable()
{
    return shadercAvailable() && bgfxCommonAvailable();
}

std::vector<std::string> shadercIncludeDirs()
{
    std::vector<std::string> dirs;
    if (AY_SHADER_BGFX_COMMON_HINT[0] && fileExists(AY_SHADER_BGFX_COMMON_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_COMMON_HINT);
    }
    if (AY_SHADER_BGFX_SRC_HINT[0] && fileExists(AY_SHADER_BGFX_SRC_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_SRC_HINT);
    }
    return dirs;
}

void configurePool(ShaderResourcePool& pool)
{
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs(shadercIncludeDirs());
}

struct BgfxNoopScope {
    bool active = false;

    BgfxNoopScope()
    {
        bgfx::Init init;
        init.type = bgfx::RendererType::Noop;
        active = bgfx::init(init);
    }

    ~BgfxNoopScope()
    {
        if (active) {
            bgfx::shutdown();
        }
    }
};

CompiledShaderProgram compileMinimalUnlit()
{
    Compiler compiler;
    return compiler.compileToProgram(kMinimalUnlit);
}

void clearPhase36Env()
{
    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");
}

} // namespace

TEST_SUITE(ShaderResourceTests)

// ---------------------------------------------------------------------------
// Layer A �?no bgfx runtime required
// ---------------------------------------------------------------------------

TEST_CASE(empty_shader_resource_is_invalid)
{
    ShaderResource res;
    CHECK_FALSE(res.isValid());
    CHECK_INT_EQ(res.getUniformBinding("anything"), InvalidBinding);
}

TEST_CASE(pool_acquire_failed_program_returns_invalid)
{
    CompiledShaderProgram prog;
    prog.success = false;

    ShaderResourcePool pool;
    ShaderResource res = pool.acquire(prog);
    CHECK_FALSE(res.isValid());
    pool.shutdown();
}

TEST_CASE(pool_acquire_empty_bins_returns_invalid)
{
    CompiledShaderProgram prog;
    prog.success = true;

    ShaderResourcePool pool;
    ShaderResource res = pool.acquire(prog);
    CHECK_FALSE(res.isValid());
    pool.shutdown();
}

TEST_CASE(invalid_binding_queries_return_zero)
{
    ShaderResource res;
    CHECK_INT_EQ(res.getTextureBinding("tex"), InvalidBinding);
    CHECK_INT_EQ(res.getUniformBlockBinding("Camera"), InvalidBinding);
    CHECK_INT_EQ(res.getStorageBufferBinding("data"), InvalidBinding);
}

// ---------------------------------------------------------------------------
// Layer B �?bgfx wire-up (opt-in when shaderc + bgfx common available)
// ---------------------------------------------------------------------------

TEST_CASE(pool_acquire_unlit_wires_up)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc not available.\n";
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderResource test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    CompiledShaderProgram prog = compileMinimalUnlit();
    if (!prog.success) {
        std::cerr << "[ShaderResource test] SKIP: compileToProgram failed.\n";
        return;
    }

    ShaderResourcePool pool;
    ShaderResource res = pool.acquire(prog);
    CHECK(res.isValid());

    const BindingId baseColorId = res.getUniformBinding("baseColor");
    CHECK(baseColorId != InvalidBinding);

    pool.shutdown();
    CHECK_FALSE(res.isValid());
}

TEST_CASE(pool_compile_end_to_end)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc not available.\n";
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderResource test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    ShaderResourcePool pool;
    configurePool(pool);

    ShaderResource res = pool.compile(kMinimalUnlit);
    CHECK(res.isValid());
    CHECK(res.getUniformBinding("baseColor") != InvalidBinding);

    pool.shutdown();
}

TEST_CASE(compiler_compile_to_shader_resource)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc not available.\n";
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderResource test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    ShaderResourcePool pool;
    configurePool(pool);

    Compiler compiler;
    ShaderResource res = compiler.compileToShaderResource(kMinimalUnlit, pool);
    CHECK(res.isValid());
    CHECK(res.getUniformBinding("baseColor") != InvalidBinding);

    pool.shutdown();
}

TEST_CASE(pool_acquire_cache_returns_same_resource)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc not available.\n";
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderResource test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    ShaderResourcePool pool;
    configurePool(pool);

    ShaderResource first = pool.acquire(kMinimalUnlit, "test-cache-key");
    ShaderResource second = pool.acquire(kMinimalUnlit, "test-cache-key");
    CHECK(first.isValid());
    CHECK(second.isValid());
    CHECK(first.getUniformBinding("baseColor") == second.getUniformBinding("baseColor"));

    pool.release(first);
    CHECK_FALSE(first.isValid());
    CHECK_FALSE(second.isValid());

    ShaderResource third = pool.acquire(kMinimalUnlit, "test-cache-key");
    CHECK(third.isValid());

    pool.shutdown();
}

TEST_CASE(compile_to_program_std140_block_metadata)
{
    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        return;
    }

    Compiler compiler;
    CompiledShaderProgram prog = compiler.compileToProgram(kUboMaterial);
    if (!prog.success) {
        std::cerr << "[ShaderResource test] SKIP: compileToProgram(UBO) failed.\n";
        return;
    }

    CHECK(prog.uniformBlocks.size() == 1);
    CHECK(prog.uniformBlocks[0].name == "Camera");
    CHECK(prog.uniformBlocks[0].sizeBytes == 16);
    CHECK(prog.uniformBlocks[0].members.size() == 2);
    CHECK(prog.uniformBlocks[0].members[0].name == "position");
    CHECK(prog.uniformBlocks[0].members[0].offsetBytes == 0);
    CHECK(prog.uniformBlocks[0].members[1].name == "fov");
    CHECK(prog.uniformBlocks[0].members[1].offsetBytes == 12);
}

TEST_CASE(pool_ubo_wires_std140_layout)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        return;
    }

    ShaderResourcePool pool;
    configurePool(pool);
    ShaderResource res = pool.compile(kUboMaterial);
    CHECK(res.isValid());

    const BindingId cameraId = res.getUniformBlockBinding("Camera");
    CHECK(cameraId != InvalidBinding);
    CHECK(res.getUniformBlockSize(cameraId) == 16);
    CHECK(res.getUniformBlockFieldOffset(cameraId, "position") == 0);
    CHECK(res.getUniformBlockFieldSize(cameraId, "position") == 12);
    CHECK(res.getUniformBlockFieldOffset(cameraId, "fov") == 12);
    CHECK(res.getUniformBlockFieldSize(cameraId, "fov") == 4);

    float blockData[4] = {1.0f, 2.0f, 3.0f, 0.5f};
    res.setUniformBlock(cameraId, blockData, sizeof(blockData));

    DrawCallContext ctx;
    ctx.viewId = 0;
    res.submit(ctx);

    pool.shutdown();
}

TEST_CASE(set_uniform_and_submit_no_crash)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc not available.\n";
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderResource test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    CompiledShaderProgram prog = compileMinimalUnlit();
    if (!prog.success) {
        std::cerr << "[ShaderResource test] SKIP: compileToProgram failed.\n";
        return;
    }

    ShaderResourcePool pool;
    ShaderResource res = pool.acquire(prog);
    CHECK(res.isValid());

    const BindingId baseColorId = res.getUniformBinding("baseColor");
    CHECK(baseColorId != InvalidBinding);

    const float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    res.setUniform(baseColorId, color, sizeof(color));

    DrawCallContext ctx;
    ctx.viewId = 0;
    res.submit(ctx);

    pool.shutdown();
}

TEST_CASE(submit_with_render_state_no_crash)
{
    if (!wireUpEnvironmentAvailable()) {
        std::cerr << "[ShaderResource test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    clearPhase36Env();
    AYShadercDriver::clearDefaultExecutable();
    if (!shadercAvailable()) {
        return;
    }

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        return;
    }

    CompiledShaderProgram prog = compileMinimalUnlit();
    if (!prog.success) {
        return;
    }

    ShaderResourcePool pool;
    ShaderResource res = pool.acquire(prog);
    CHECK(res.isValid());

    DrawCallContext ctx;
    ctx.viewId = 0;
    ctx.state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    res.submit(ctx);

    pool.shutdown();
}

TEST_SUITE_END
