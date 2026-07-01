#pragma once
// AYShaderResourcePool.h - ShaderResource factory + engine config (Phase 4-B+)

#include "AYShaderResource.h"
#include "AYShaderProgram.h"
#include "detail/AYShaderCapability.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ayt::shader::phoskia
{
struct CompileOptions;
}

namespace ayt::shader
{

class ShaderResourceImpl;

struct CacheStats {
    uint64_t sourceHits = 0;
    uint64_t sourceMisses = 0;
    uint64_t binaryHits = 0;
    uint64_t binaryMisses = 0;
};

class ShaderResourcePool {
public:
    ShaderResourcePool();
    ~ShaderResourcePool();

    ShaderResourcePool(const ShaderResourcePool&) = delete;
    ShaderResourcePool& operator=(const ShaderResourcePool&) = delete;
    ShaderResourcePool(ShaderResourcePool&&) noexcept;
    ShaderResourcePool& operator=(ShaderResourcePool&&) noexcept;

    void setShadercExecutable(const std::string& path);
    void setBgfxIncludeDirs(const std::vector<std::string>& dirs);
    void setPlatform(const std::string& platform);
    void setGLSLProfile(const std::string& profile);
    void setCacheDirectory(const std::string& path);
    void setHotReloadEnabled(bool enabled);

    void require(ShaderCapability capability);
    void setAutoProbeFromRendererType(bool enabled);
    void bindRendererTypeForTests(uint8_t bgfxRendererType, const std::string& platform,
                                  const std::string& profile);

    CacheStats cacheStats() const;

    ShaderResource compile(const std::string& src);
    ShaderResource compile(const std::string& src,
                           const phoskia::CompileOptions& opts);
    ShaderResource compileFromFile(const std::string& path);
    ShaderResource compileFromFile(const std::string& path,
                                   const phoskia::CompileOptions& opts);
    void pollHotReload();

    ShaderResource acquire(const CompiledShaderProgram& prog);
    ShaderResource acquire(const std::string& src,
                           const std::string& cacheKey = "");
    ShaderResource acquire(const std::string& src,
                           const phoskia::CompileOptions& opts,
                           const std::string& cacheKey = "");

    void release(ShaderResource& res);
    void shutdown();

    static ShaderResourceImpl* resolveHandle(uint64_t handle);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::shader
