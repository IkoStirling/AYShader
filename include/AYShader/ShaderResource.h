#pragma once
// AYShader/ShaderResource.h - Opaque shader resource handle (Phase 4-A/O)

#include <cstdint>
#include <functional>
#include <string>

namespace ayt::shader
{

using BindingId = uint32_t;
constexpr BindingId InvalidBinding = 0;

struct TextureHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

struct DrawCallContext {
    uint8_t viewId = 0;
    // A non-zero value is applied for this draw. Zero means the owning
    // RenderPass already configured bgfx state through its adapter; submit()
    // preserves that state across consecutive draws in the same pass while
    // still discarding per-draw bindings, buffers and transforms.
    uint64_t state = 0;
};

class ShaderResourcePool;

class ShaderResource {
public:
    ShaderResource() noexcept = default;
    explicit ShaderResource(uint64_t id) noexcept : _id(id) {}

    bool isValid() const noexcept;
    void reset() noexcept { _id = 0; }

    bool operator==(const ShaderResource& other) const noexcept { return _id == other._id; }
    bool operator!=(const ShaderResource& other) const noexcept { return _id != other._id; }

    BindingId getUniformBinding(const std::string& name) const;
    BindingId getTextureBinding(const std::string& name) const;
    // SAMPLER2D(name, slot) unit recorded at compile time. 0 if unknown.
    uint8_t getTextureStage(BindingId id) const;
    BindingId getUniformBlockBinding(const std::string& name) const;
    BindingId getStorageBufferBinding(const std::string& name) const;

    // True when `id` is a uniform/UBO binding on this resource (not a texture slot).
    bool hasUniformBinding(BindingId id) const;
    bool hasTextureBinding(BindingId id) const;

    size_t getUniformBlockSize(BindingId blockId) const;
    size_t getUniformBlockFieldOffset(BindingId blockId,
                                      const std::string& fieldName) const;
    size_t getUniformBlockFieldSize(BindingId blockId,
                                    const std::string& fieldName) const;

    // Copies the value immediately. Array bindings accept a prefix of the
    // reflected capacity and submit only the supplied elements; short scalar
    // values are padded to one backend Vec4 slot.
    void setUniform(BindingId id, const void* data, size_t sizeBytes) const;
    // General blocks require their complete std140 payload. A block containing
    // only mat4[N] additionally accepts a compact prefix (used by skin palettes).
    void setUniformBlock(BindingId blockId, const void* data, size_t sizeBytes) const;
    // Binds `tex` to this sampler. The texture unit is taken from the
    // shader's SAMPLER2D(name, slot) — the `stage` argument is ignored
    // when the binding has a recorded slot (preferred). Passing a wrong
    // hard-coded stage (e.g. always 0 for albedo, always 1 for shadow)
    // swaps maps and turns R32F shadow depth into grayscale "albedo".
    void setTexture(uint8_t stage, BindingId id, const TextureHandle& tex) const;
    void submit(const DrawCallContext& ctx) const;

    uint64_t id() const noexcept { return _id; }

private:
    friend class ShaderResourcePool;
    uint64_t _id = 0;
};

} // namespace ayt::shader

namespace std
{
template <>
struct hash<ayt::shader::ShaderResource> {
    size_t operator()(const ayt::shader::ShaderResource& res) const noexcept
    {
        return std::hash<uint64_t>{}(res.id());
    }
};
} // namespace std
