// Test_ShaderDiskCache.cpp ??Phase 4-I disk cache tier

#include "AYPhoskia.h"
#include "AYShaderResourcePool.h"
#include "AYShadercDriver.h"
#include "AYTest.h"
#include "detail/AYShaderDigest.h"
#include "detail/AYShaderDiskCache.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
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
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
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

inline bool fileExists(const std::string& p)
{
    if (p.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string uniqueTempCacheDir()
{
    char buf[512];
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    if (!t) {
        t = "C:\\Temp";
    }
    std::snprintf(buf, sizeof(buf), "%s\\ayshader_disk_%u",
                  t, static_cast<unsigned>(GetCurrentProcessId()));
    _mkdir(buf);
#else
    const char* t = std::getenv("TMPDIR");
    if (!t) {
        t = "/tmp";
    }
    std::snprintf(buf, sizeof(buf), "%s/ayshader_disk_%u",
                  t, static_cast<unsigned>(::getpid()));
    ::mkdir(buf, 0755);
#endif
    return buf;
}

CompiledShaderProgram makeSampleProgram()
{
    CompiledShaderProgram prog;
    prog.success = true;
    prog.vsBin = {0x01, 0x02, 0x03, 0x04};
    prog.fsBin = {0x10, 0x20, 0x30};

    BGFXUniformBlock block;
    block.name = "Camera";
    block.binding = 0;
    block.sizeBytes = 16;
    block.fieldNames = {"position", "fov"};
    block.members = {
        {"position", "vec3", 0, 12},
        {"fov", "float", 12, 4},
    };
    prog.uniformBlocks.push_back(std::move(block));

    BGFXUniform uniform;
    uniform.name = "baseColor";
    uniform.type = "vec4";
    uniform.count = 1;
    prog.uniforms.push_back(std::move(uniform));

    BGFXTexture texture;
    texture.name = "albedo";
    texture.binding = 0;
    texture.textureType = "sampler2D";
    prog.textures.push_back(std::move(texture));

    BGFXStorageBuffer storage;
    storage.name = "instances";
    storage.binding = 1;
    storage.elementType = "InstanceData";
    prog.storageBuffers.push_back(std::move(storage));

    return prog;
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

void configurePool(ShaderResourcePool& pool, const std::string& cacheDir)
{
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs(shadercIncludeDirs());
    pool.setAutoProbeFromRendererType(false);
    pool.setPlatform("windows");
    pool.setGLSLProfile("430");
    pool.setCacheDirectory(cacheDir);
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

TEST_SUITE(ShaderDiskCacheTests)

TEST_CASE(disk_cache_roundtrip_preserves_program)
{
    const CompiledShaderProgram original = makeSampleProgram();
    const std::string path = uniqueTempCacheDir() + "/roundtrip.aysc";

    CHECK(saveCompiledProgramToDisk(path, original));
    CHECK(fileExists(path));

    CompiledShaderProgram loaded;
    CHECK(loadCompiledProgramFromDisk(path, loaded));
    CHECK(loaded.success);
    CHECK(loaded.vsBin == original.vsBin);
    CHECK(loaded.fsBin == original.fsBin);
    CHECK(loaded.csBin == original.csBin);
    CHECK(loaded.uniformBlocks.size() == 1);
    CHECK(loaded.uniformBlocks[0].name == "Camera");
    CHECK(loaded.uniformBlocks[0].sizeBytes == 16);
    CHECK(loaded.uniformBlocks[0].members.size() == 2);
    CHECK(loaded.uniformBlocks[0].members[1].offsetBytes == 12);
    CHECK(loaded.uniforms.size() == 1);
    CHECK(loaded.uniforms[0].name == "baseColor");
    CHECK(loaded.textures.size() == 1);
    CHECK(loaded.textures[0].name == "albedo");
    CHECK(loaded.storageBuffers.size() == 1);
    CHECK(loaded.storageBuffers[0].name == "instances");
}

TEST_CASE(disk_cache_rejects_bad_magic)
{
    const std::string path = uniqueTempCacheDir() + "/bad_magic.aysc";
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        const char bytes[] = {'B', 'A', 'D', '!', 1, 0, 0, 0, 1};
        std::fwrite(bytes, 1, sizeof(bytes), f);
        std::fclose(f);
    }

    CompiledShaderProgram loaded;
    CHECK_FALSE(loadCompiledProgramFromDisk(path, loaded));
}

TEST_CASE(pool_disk_cache_persists_across_pool_instances)
{
    if (!shadercAvailable() || !bgfxCommonAvailable()) {
        std::cerr << "[ShaderDiskCache test] SKIP: shaderc/bgfx common not available.\n";
        return;
    }

    PUTENV_S("AY_PHOSKIA_KEEP_SOURCES", "");
    PUTENV_S("AY_PHOSKIA_DUMP_SC", "");

    BgfxNoopScope bgfxScope;
    if (!bgfxScope.active) {
        std::cerr << "[ShaderDiskCache test] SKIP: bgfx::init(Noop) failed.\n";
        return;
    }

    const std::string cacheDir = uniqueTempCacheDir() + "/pool";

    ShaderResourcePool poolA;
    configurePool(poolA, cacheDir);
    ShaderResource resA = poolA.compile(kMinimalUnlit);
    CHECK(resA.isValid());
    poolA.shutdown();

    {
        std::ostringstream oss;
        oss << "windows|430|" << AY_SHADER_SHADERC_HINT << "|";
        for (const std::string& dir : shadercIncludeDirs()) {
            oss << dir << ';';
        }
        oss << "|15||00|" << kMinimalUnlit;
        const std::string cacheFile =
            diskCacheFilePath(cacheDir, sha256Hex(oss.str()));
        CHECK(fileExists(cacheFile));
    }

    ShaderResourcePool poolB;
    configurePool(poolB, cacheDir);
    ShaderResource resB = poolB.compile(kMinimalUnlit);
    CHECK(resB.isValid());
    CHECK(resB.getUniformBinding("baseColor") != InvalidBinding);
    poolB.shutdown();
}

TEST_SUITE_END
