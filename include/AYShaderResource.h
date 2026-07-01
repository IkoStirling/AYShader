#pragma once
// AYShaderResource.h - Opaque shader resource handle (Phase 4-A)
//
// Frontend-facing API for wired-up shader programs. All bgfx types live
// behind ShaderResourceImpl (pimpl); this header must not include bgfx.

#include <cstdint>
#include <memory>
#include <string>

namespace ayt::shader
{

using BindingId = uint32_t;
constexpr BindingId InvalidBinding = 0;

// Driver-neutral texture reference. For the bgfx backend the low 16 bits
// carry bgfx::TextureHandle.idx; AYRenderer is expected to populate this
// when handing textures to ShaderResource::setTexture.
struct TextureHandle {
    uint64_t id = 0;

    bool isValid() const noexcept { return id != 0; }
};

// Minimal draw-call context for Phase 4-A submit(). Phase 4-F extends
// this for full AYRenderer integration.
struct DrawCallContext {
    uint8_t viewId = 0;
};

class ShaderResourcePool;

class ShaderResourceImpl;

class ShaderResource {
public:
    ShaderResource();
    ~ShaderResource();

    ShaderResource(const ShaderResource&) = default;
    ShaderResource& operator=(const ShaderResource&) = default;
    ShaderResource(ShaderResource&&) noexcept = default;
    ShaderResource& operator=(ShaderResource&&) noexcept = default;

    bool isValid() const noexcept;

    BindingId getUniformBinding(const std::string& name) const;
    BindingId getTextureBinding(const std::string& name) const;
    BindingId getUniformBlockBinding(const std::string& name) const;
    BindingId getStorageBufferBinding(const std::string& name) const;

    void setUniform(BindingId id, const void* data, size_t sizeBytes) const;
    void setTexture(uint8_t stage, BindingId id, const TextureHandle& tex) const;
    void submit(const DrawCallContext& ctx) const;

private:
    friend class ShaderResourcePool;

    explicit ShaderResource(std::shared_ptr<ShaderResourceImpl> impl);

    std::shared_ptr<ShaderResourceImpl> _impl;
};

} // namespace ayt::shader
