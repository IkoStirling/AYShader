#pragma once
// AYShaderResourcePool.h - ShaderResource factory + engine config (Phase 4-B+)
//
// Engine startup configures shaderc path / include dirs / platform once;
// per-shader calls use compile() with optional phoskia::CompileOptions.

#include "AYShaderResource.h"
#include "AYShaderProgram.h"

#include <memory>
#include <string>
#include <vector>

namespace ayt::shader::phoskia
{
struct CompileOptions;
}

namespace ayt::shader
{

class ShaderResourcePool {
public:
    ShaderResourcePool();
    ~ShaderResourcePool();

    ShaderResourcePool(const ShaderResourcePool&) = delete;
    ShaderResourcePool& operator=(const ShaderResourcePool&) = delete;
    ShaderResourcePool(ShaderResourcePool&&) noexcept;
    ShaderResourcePool& operator=(ShaderResourcePool&&) noexcept;

    // --- Engine config (Phase 4-B) ---
    void setShadercExecutable(const std::string& path);
    void setBgfxIncludeDirs(const std::vector<std::string>& dirs);
    void setPlatform(const std::string& platform);
    void setGLSLProfile(const std::string& profile);
    void setCacheDirectory(const std::string& path);
    void setHotReloadEnabled(bool enabled);

    // --- Primary frontend path (Phase 4-B/C) ---
    ShaderResource compile(const std::string& src);
    ShaderResource compile(const std::string& src,
                           const phoskia::CompileOptions& opts);
    ShaderResource compileFromFile(const std::string& path);
    ShaderResource compileFromFile(const std::string& path,
                                   const phoskia::CompileOptions& opts);

    // Dev-only hot-reload poll (no-op when setHotReloadEnabled(false)).
    void pollHotReload();

    // Lower-level: wire an already-compiled program (Phase 4-A API).
    ShaderResource acquire(const CompiledShaderProgram& prog);

    ShaderResource acquire(const std::string& src,
                           const std::string& cacheKey = "");
    ShaderResource acquire(const std::string& src,
                           const phoskia::CompileOptions& opts,
                           const std::string& cacheKey = "");

    void release(ShaderResource& res);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::shader
