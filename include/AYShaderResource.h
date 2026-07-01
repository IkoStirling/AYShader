#pragma once
// AYShaderResource.h - Opaque shader resource handle (Phase 4-A/O)

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
    BindingId getUniformBlockBinding(const std::string& name) const;
    BindingId getStorageBufferBinding(const std::string& name) const;

    size_t getUniformBlockSize(BindingId blockId) const;
    size_t getUniformBlockFieldOffset(BindingId blockId,
                                      const std::string& fieldName) const;
    size_t getUniformBlockFieldSize(BindingId blockId,
                                    const std::string& fieldName) const;

    void setUniform(BindingId id, const void* data, size_t sizeBytes) const;
    void setUniformBlock(BindingId blockId, const void* data, size_t sizeBytes) const;
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
