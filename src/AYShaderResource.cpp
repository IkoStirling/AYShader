// AYShaderResource.cpp - ShaderResource public API forwarding

#include "AYShaderResource.h"
#include "ShaderResourceImpl.h"

#include <cstring>

namespace ayt::shader
{

ShaderResource::ShaderResource() = default;

ShaderResource::ShaderResource(std::shared_ptr<ShaderResourceImpl> impl)
    : _impl(std::move(impl))
{
}

ShaderResource::~ShaderResource() = default;

bool ShaderResource::isValid() const noexcept
{
    return _impl && bgfx::isValid(_impl->programHandle);
}

namespace {

BindingId lookupBinding(const std::unordered_map<std::string, BindingId>& table,
                        const std::string& name)
{
    const auto it = table.find(name);
    return it != table.end() ? it->second : InvalidBinding;
}

const BindingEntry* findBindingEntry(const ShaderResourceImpl& impl, BindingId id)
{
    const auto it = impl.bindingsById.find(id);
    return it != impl.bindingsById.end() ? &it->second : nullptr;
}

} // namespace

BindingId ShaderResource::getUniformBinding(const std::string& name) const
{
    if (!_impl) {
        return InvalidBinding;
    }
    return lookupBinding(_impl->uniformBindings, name);
}

BindingId ShaderResource::getTextureBinding(const std::string& name) const
{
    if (!_impl) {
        return InvalidBinding;
    }
    return lookupBinding(_impl->textureBindings, name);
}

BindingId ShaderResource::getUniformBlockBinding(const std::string& name) const
{
    if (!_impl) {
        return InvalidBinding;
    }
    return lookupBinding(_impl->uniformBlockBindings, name);
}

BindingId ShaderResource::getStorageBufferBinding(const std::string& name) const
{
    if (!_impl) {
        return InvalidBinding;
    }
    return lookupBinding(_impl->storageBufferBindings, name);
}

void ShaderResource::setUniform(BindingId id, const void* data, size_t sizeBytes) const
{
    if (!_impl || id == InvalidBinding || data == nullptr || sizeBytes == 0) {
        return;
    }

    const BindingEntry* entry = findBindingEntry(*_impl, id);
    if (entry == nullptr) {
        return;
    }
    if (entry->kind != BindingKind::Uniform && entry->kind != BindingKind::UniformBlock) {
        return;
    }

    PendingUniform pending;
    pending.id = id;
    pending.data.resize(sizeBytes);
    std::memcpy(pending.data.data(), data, sizeBytes);
    _impl->pendingUniforms.push_back(std::move(pending));
}

void ShaderResource::setTexture(uint8_t stage, BindingId id, const TextureHandle& tex) const
{
    if (!_impl || id == InvalidBinding || !tex.isValid()) {
        return;
    }

    const BindingEntry* entry = findBindingEntry(*_impl, id);
    if (entry == nullptr || entry->kind != BindingKind::Texture) {
        return;
    }

    PendingTexture pending;
    pending.stage = stage;
    pending.id = id;
    pending.texture.idx = static_cast<uint16_t>(tex.id & 0xFFFFu);
    _impl->pendingTextures.push_back(pending);
}

void ShaderResource::submit(const DrawCallContext& ctx) const
{
    if (!_impl || !bgfx::isValid(_impl->programHandle)) {
        return;
    }

    for (const PendingUniform& pending : _impl->pendingUniforms) {
        const BindingEntry* entry = findBindingEntry(*_impl, pending.id);
        if (entry == nullptr || !bgfx::isValid(entry->uniformHandle)) {
            continue;
        }
        if (pending.data.empty()) {
            continue;
        }
        bgfx::setUniform(entry->uniformHandle, pending.data.data());
    }

    for (const PendingTexture& pending : _impl->pendingTextures) {
        const BindingEntry* entry = findBindingEntry(*_impl, pending.id);
        if (entry == nullptr || !bgfx::isValid(entry->uniformHandle)) {
            continue;
        }
        if (!bgfx::isValid(pending.texture)) {
            continue;
        }
        bgfx::setTexture(pending.stage, entry->uniformHandle, pending.texture);
    }

    bgfx::submit(ctx.viewId, _impl->programHandle);

    _impl->pendingUniforms.clear();
    _impl->pendingTextures.clear();
}

} // namespace ayt::shader
