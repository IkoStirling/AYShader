// AYShaderResource.cpp - ShaderResource public API (Phase 4-O handle table)

#include "AYShader/ShaderResource.h"
#include "AYShader/ShaderResourcePool.h"
#include "ShaderResourceImpl.h"

#include <cstdio>
#include <cstring>

namespace ayt::shader
{

namespace {

std::shared_ptr<ShaderResourceImpl> resolveImpl(uint64_t handle)
{
    return ShaderResourcePool::retainHandle(handle);
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
    auto impl = resolveImpl(_id);
    return impl != nullptr && bgfx::isValid(impl->programHandle);
}

BindingId ShaderResource::getUniformBinding(const std::string& name) const
{
    auto impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->uniformBindings, name);
}

BindingId ShaderResource::getTextureBinding(const std::string& name) const
{
    auto impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->textureBindings, name);
}

uint8_t ShaderResource::getTextureStage(BindingId id) const
{
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
    if (impl == nullptr) {
        return InvalidBinding;
    }
    return lookupBinding(impl->uniformBlockBindings, name);
}

BindingId ShaderResource::getStorageBufferBinding(const std::string& name) const
{
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
    if (impl == nullptr) {
        return false;
    }
    const BindingEntry* entry = findBindingEntry(*impl, id);
    return entry != nullptr && entry->kind == BindingKind::Texture;
}

size_t ShaderResource::getUniformBlockSize(BindingId blockId) const
{
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
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
    const size_t elementBytes = entry->uniformElementSizeBytes != 0u
        ? entry->uniformElementSizeBytes
        : 16u;
    const size_t capacityBytes =
        elementBytes * static_cast<size_t>(entry->uniformSubmitCount);
    if (sizeBytes > capacityBytes) {
        std::fprintf(stderr,
                     "[ShaderResource] setUniform('%s') overflow: "
                     "got %zu bytes, capacity %zu — upload skipped\n",
                     entry->name.c_str(), sizeBytes, capacityBytes);
        return;
    }

    const size_t requestedCount = (sizeBytes + elementBytes - 1u) / elementBytes;
    pending.submitCount = static_cast<uint16_t>(requestedCount);
    // bgfx consumes complete Vec4/Mat3/Mat4 elements.  Zero-fill the tail for
    // scalar values represented by a Vec4 slot instead of allowing bgfx to
    // read past a short caller buffer.
    pending.data.resize(requestedCount * elementBytes, uint8_t{0});
    std::memcpy(pending.data.data(), data, sizeBytes);
    impl->pendingUniforms.push_back(std::move(pending));
}

void ShaderResource::setUniformBlock(BindingId blockId,
                                     const void* data,
                                     size_t sizeBytes) const
{
    auto impl = resolveImpl(_id);
    if (impl == nullptr || blockId == InvalidBinding || data == nullptr || sizeBytes == 0) {
        return;
    }

    const BindingEntry* entry = findBindingEntry(*impl, blockId);
    if (entry == nullptr || entry->kind != BindingKind::UniformBlock) {
        return;
    }
    const bool compactMat4ArrayWrite =
        entry->uniformElementSizeBytes == 64u
        && sizeBytes <= entry->uniformBlockSizeBytes
        && (sizeBytes % 64u) == 0u;
    if (entry->uniformBlockSizeBytes != 0
        && sizeBytes != entry->uniformBlockSizeBytes
        && !compactMat4ArrayWrite) {
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
    auto impl = resolveImpl(_id);
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
    auto impl = resolveImpl(_id);
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
        if (pending.submitCount == 0u
            || pending.submitCount > entry->uniformSubmitCount) {
            continue;
        }
        bgfx::setUniform(entry->uniformHandle, pending.data.data(),
                         pending.submitCount);
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

    uint8_t discardFlags = BGFX_DISCARD_ALL;
    if (ctx.state != 0) {
        bgfx::setState(ctx.state);
    } else {
        // Render passes commonly set one shared state before a multi-draw
        // loop and pass state==0 here. bgfx::submit defaults to DISCARD_ALL;
        // using that default made only the first submesh retain WRITE/CULL/
        // DEPTH state. Preserve only render state between those submissions;
        // all per-draw bindings, buffers and transforms remain discarded and
        // are explicitly rebound by the next item.
        discardFlags = static_cast<uint8_t>(BGFX_DISCARD_ALL
                                           & ~BGFX_DISCARD_STATE);
    }

    bgfx::submit(ctx.viewId, impl->programHandle, 0, discardFlags);

    impl->pendingUniforms.clear();
    impl->pendingTextures.clear();
}

} // namespace ayt::shader
