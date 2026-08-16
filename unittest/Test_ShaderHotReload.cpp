// Test_ShaderHotReload.cpp �?Phase 4-J hot-reload watch + debounce

#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShadercDriver.h"
#include "AYTest.h"
#include "AYShader/detail/ShaderFileWatch.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <chrono>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#  define PUTENV_S(name, val) _putenv_s(name, val)
#else
#  include <unistd.h>
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
using namespace ayt::shader::detail;
using namespace ayt::shader::phoskia;

namespace {

const std::string kShaderV1 = R"(material Unlit {
    property baseColor = vec4(1.0, 0.0, 0.0, 1.0);
    vertex {
        in position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in position : position;
        return baseColor;
    }
}
)";

const std::string kShaderV2 = R"(material Unlit {
    property baseColor = vec4(0.0, 1.0, 0.0, 1.0);
    vertex {
        in position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in position : position;
        return baseColor;
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

bool writeTextFile(const std::string& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

std::string uniqueTempDir()
{
    char buf[512];
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    if (!t) {
        t = "C:\\Temp";
    }
    std::snprintf(buf, sizeof(buf), "%s\\ayshader_hotreload_%u",
                  t, static_cast<unsigned>(GetCurrentProcessId()));
    _mkdir(buf);
#else
    const char* t = std::getenv("TMPDIR");
    if (!t) {
        t = "/tmp";
    }
    std::snprintf(buf, sizeof(buf), "%s/ayshader_hotreload_%u",
                  t, static_cast<unsigned>(::getpid()));
    ::mkdir(buf, 0755);
#endif
    return buf;
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

} // namespace

TEST_SUITE(ShaderHotReloadTests)

TEST_CASE(file_watch_helpers_roundtrip)
{
    const std::string dir = uniqueTempDir();
    const std::string path = dir + "/sample.phoskia";
    CHECK(writeTextFile(path, kShaderV1));

    std::string loaded;
    CHECK(readTextFile(path, loaded));
    CHECK(loaded == kShaderV1);
    CHECK(normalizeSourcePath(R"(foo\bar\baz.phoskia)") == "foo/bar/baz.phoskia");
    CHECK(fileMtimeMs(path).has_value());
}

TEST_CASE(hot_reload_debounce_invalidates_resource)
{
    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[ShaderHotReload test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderHotReload test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    const std::string path = uniqueTempDir() + "/hotreload.phoskia";
    CHECK(writeTextFile(path, kShaderV1));

    ShaderResourcePool pool;
    configurePool(pool);
    pool.setHotReloadEnabled(true);

    ShaderResource res = pool.compileFromFile(path);
    CHECK(res.isValid());

    CHECK(writeTextFile(path, kShaderV2));

    bool invalidated = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        pool.pollHotReload();
        if (!res.isValid()) {
            invalidated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(invalidated);

    ShaderResource reloaded = pool.compileFromFile(path);
    CHECK(reloaded.isValid());
    CHECK(reloaded.getUniformBinding("baseColor") != InvalidBinding);

    pool.shutdown();
}

TEST_CASE(hot_reload_disabled_is_noop)
{
    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        return;
    }

    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        return;
    }

    const std::string path = uniqueTempDir() + "/disabled.phoskia";
    CHECK(writeTextFile(path, kShaderV1));

    ShaderResourcePool pool;
    configurePool(pool);
    pool.setHotReloadEnabled(false);

    ShaderResource res = pool.compileFromFile(path);
    CHECK(res.isValid());

    CHECK(writeTextFile(path, kShaderV2));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    pool.pollHotReload();
    CHECK(res.isValid());

    pool.shutdown();
}

TEST_SUITE_END
