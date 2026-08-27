#pragma once
// AYShader/ShaderResourcePool.h - ShaderResource factory + engine config (Phase 4-B+)

#include "AYShader/ShaderResource.h"
#include "AYShader/ShaderProgram.h"
#include "AYShader/detail/ShaderCapability.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
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

    // When non-empty, enables keepSources + dumpIntermediate and writes
    // vs/fs/varying.def.sc under this directory on each compile (best-effort).
    void setIntermediateDumpDirectory(const std::string& path);

    void require(ShaderCapability capability);
    void setAutoProbeFromRendererType(bool enabled);
    // Call after bgfx::init so the first acquire uses the active renderer
    // (e.g. D3D11 -> windows / s_5_0, not the stale GLSL 430 default).
    void resolvePlatformFromRenderer();
    void bindRendererTypeForTests(uint8_t bgfxRendererType, const std::string& platform,
                                  const std::string& profile);

    CacheStats cacheStats() const;
    const std::vector<std::string>& lastCompileErrors() const;

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

    // Compile raw bgfx .sc sources (non-Phoskia) with the same shaderc options as engine materials.
    ShaderResource acquireFromBgfxSc(const std::string& vertexSc,
                                     const std::string& fragmentSc,
                                     const std::string& varyingDefSc,
                                     const std::string& cacheKey = "bgfx_sc");

    void release(ShaderResource& res);
    void shutdown();

    static ShaderResourceImpl* resolveHandle(uint64_t handle);
    static std::shared_ptr<ShaderResourceImpl> retainHandle(uint64_t handle);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    // R-B-02 audit helper (2026-08-26): the only sanctioned path that
    // mutates `_impl` and returns a live mutex lock.  Declared here as a
    // private member so it can hold `_impl` exclusively without leaking
    // its type.  Every setter / mutating operation in
    // `AYShaderResourcePool.cpp` calls this method instead of touching
    // `_impl` directly, which keeps the lock acquisition in one place.
    std::unique_lock<std::shared_mutex> lockOrCreateImplExclusive();
};

} // namespace ayt::shader
