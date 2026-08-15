// AYShaderResource.cpp - ShaderResource public API (Phase 4-O handle table)

#include "AYShader/ShaderResource.h"
#include "AYShader/ShaderResourcePool.h"
#include "ShaderResourceImpl.h"

#include <cstdio>
#include <cstring>

namespace ayt::shader
{

namespace {

ShaderResourceImpl* resolveImpl(uint64_t handle)
{
    return ShaderResourcePool::resolveHandle(handle);
}

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

bool ShaderResource::isValid() const noexcept
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    return impl != nullptr && bgfx::isValid(impl->programHandle);
}

BindingId ShaderResource::getUniformBinding(const std::string& name) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->uniformBindings, name);
}

BindingId ShaderResource::getTextureBinding(const std::string& name) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->textureBindings, name);
}

uint8_t ShaderResource::getTextureStage(BindingId id) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || id == InvalidBinding) {
        return 0;
    }
    const BindingEntry* entry = findBindingEntry(*impl, id);
    if (entry == nullptr || entry->kind != BindingKind::Texture) {
        return 0;
    }
    return entry->textureBinding;
}

BindingId ShaderResource::getUniformBlockBinding(const std::string& name) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->uniformBlockBindings, name);
}

BindingId ShaderResource::getStorageBufferBinding(const std::string& name) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->storageBufferBindings, name);
}

bool ShaderResource::hasUniformBinding(BindingId id) const
{
    if (id == InvalidBinding) {
        return false;
    }
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr) {
        return false;
    }
    const BindingEntry* entry = findBindingEntry(*impl, id);
    return entry != nullptr
        && (entry->kind == BindingKind::Uniform || entry->kind == BindingKind::UniformBlock);
}

bool ShaderResource::hasTextureBinding(BindingId id) const
{
    if (id == InvalidBinding) {
        return false;
    }
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr) {
        return false;
    }
    const BindingEntry* entry = findBindingEntry(*impl, id);
    return entry != nullptr && entry->kind == BindingKind::Texture;
}

size_t ShaderResource::getUniformBlockSize(BindingId blockId) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || blockId == InvalidBinding) {
        return 0;
    }
    const BindingEntry* entry = findBindingEntry(*impl, blockId);
    if (entry == nullptr || entry->kind != BindingKind::UniformBlock) {
        return 0;
    }
    return entry->uniformBlockSizeBytes;
}

size_t ShaderResource::getUniformBlockFieldOffset(BindingId blockId,
                                                  const std::string& fieldName) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || blockId == InvalidBinding) {
        return 0;
    }
    const BindingEntry* entry = findBindingEntry(*impl, blockId);
    if (entry == nullptr || entry->kind != BindingKind::UniformBlock) {
        return 0;
    }
    const auto it = entry->uniformBlockFieldOffsets.find(fieldName);
    return it != entry->uniformBlockFieldOffsets.end() ? it->second : 0;
}

size_t ShaderResource::getUniformBlockFieldSize(BindingId blockId,
                                                const std::string& fieldName) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || blockId == InvalidBinding) {
        return 0;
    }
    const BindingEntry* entry = findBindingEntry(*impl, blockId);
    if (entry == nullptr || entry->kind != BindingKind::UniformBlock) {
        return 0;
    }
    const auto it = entry->uniformBlockFieldSizes.find(fieldName);
    return it != entry->uniformBlockFieldSizes.end() ? it->second : 0;
}

void ShaderResource::setUniform(BindingId id, const void* data, size_t sizeBytes) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || id == InvalidBinding || data == nullptr || sizeBytes == 0) {
        return;
    }

    const BindingEntry* entry = findBindingEntry(*impl, id);
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
    impl->pendingUniforms.push_back(std::move(pending));
}

void ShaderResource::setUniformBlock(BindingId blockId,
                                     const void* data,
                                     size_t sizeBytes) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || blockId == InvalidBinding || data == nullptr || sizeBytes == 0) {
        return;
    }

    const BindingEntry* entry = findBindingEntry(*impl, blockId);
    if (entry == nullptr || entry->kind != BindingKind::UniformBlock) {
        return;
    }
    if (entry->uniformBlockSizeBytes != 0 && sizeBytes != entry->uniformBlockSizeBytes) {
        std::fprintf(stderr,
                     "[ShaderResource] setUniformBlock('%s') size mismatch: "
                     "got %zu bytes, expected %zu — upload skipped\n",
                     entry->name.c_str(), sizeBytes, entry->uniformBlockSizeBytes);
        return;
    }

    setUniform(blockId, data, sizeBytes);
}

void ShaderResource::setTexture(uint8_t stage, BindingId id, const TextureHandle& tex) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || id == InvalidBinding || !tex.isValid()) {
        return;
    }

    const BindingEntry* entry = findBindingEntry(*impl, id);
    if (entry == nullptr || entry->kind != BindingKind::Texture) {
        return;
    }

    PendingTexture pending;
    // Always use the SAMPLER2D slot recorded at compile time. Do not
    // rename-remap by texture name — that desyncs from the .bin when
    // declaration order differs from albedo=0/shadow=1 assumptions.
    pending.stage = entry->textureBinding;
    if (stage != pending.stage && stage != 0) {
        // Caller may pass an explicit stage for single-texture programs
        // (UI / post). Prefer recorded slot when it is non-zero or the
        // name is a known multi-map material sampler.
        if (entry->name != "albedoMap" && entry->name != "shadowMap") {
            pending.stage = stage;
        }
    }
    pending.id = id;
    pending.texture.idx = static_cast<uint16_t>((tex.id - 1u) & 0xFFFFu);
    impl->pendingTextures.push_back(pending);
}

void ShaderResource::submit(const DrawCallContext& ctx) const
{
    ShaderResourceImpl* impl = resolveImpl(_id);
    if (impl == nullptr || !bgfx::isValid(impl->programHandle)) {
        return;
    }

    for (const PendingUniform& pending : impl->pendingUniforms) {
        const BindingEntry* entry = findBindingEntry(*impl, pending.id);
        if (entry == nullptr || !bgfx::isValid(entry->uniformHandle)) {
            continue;
        }
        if (pending.data.empty()) {
            continue;
        }
        bgfx::setUniform(entry->uniformHandle, pending.data.data(),
                         entry->uniformSubmitCount);
    }

    for (const PendingTexture& pending : impl->pendingTextures) {
        const BindingEntry* entry = findBindingEntry(*impl, pending.id);
        if (entry == nullptr || !bgfx::isValid(entry->uniformHandle)) {
            continue;
        }
        if (!bgfx::isValid(pending.texture)) {
            continue;
        }
        bgfx::setTexture(pending.stage, entry->uniformHandle, pending.texture);
    }

    if (ctx.state != 0) {
        bgfx::setState(ctx.state);
    }

    bgfx::submit(ctx.viewId, impl->programHandle);

    impl->pendingUniforms.clear();
    impl->pendingTextures.clear();
}

} // namespace ayt::shader
