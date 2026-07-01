#pragma once
// AYShaderResourcePool.h - ShaderResource factory + lifetime (Phase 4-A skeleton)
//
// Phase 4-B expands this with cache, hot-reload, and engine config.
// Phase 4-A provides acquire() wire-up from CompiledShaderProgram and
// shutdown() for bgfx handle cleanup.

#include "AYShaderResource.h"
#include "AYShaderProgram.h"

#include <memory>

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

    // Wire CompiledShaderProgram bytes + binding metadata into bgfx and
    // return an opaque handle. Returns an invalid ShaderResource on failure.
    ShaderResource acquire(const CompiledShaderProgram& prog);

    // Destroy all bgfx resources owned by this pool. Call before bgfx::shutdown().
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::shader
