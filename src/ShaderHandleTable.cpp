// ShaderHandleTable.cpp — opaque handle registry (Phase 4-O)

#include "detail/AYShaderHandleTable.h"
#include "AYShaderResource.h"
#include "ShaderResourceImpl.h"

namespace ayt::shader::detail
{

uint32_t ShaderHandleTable::insert(std::unique_ptr<ShaderResourceImpl> impl)
{
    if (!impl) {
        return 0;
    }
    evictIfNeeded();
    const uint32_t id = _nextId++;
    Entry& entry = _entries[id];
    entry.impl = std::move(impl);
    entry.lruStamp = ++_lruCounter;
    return id;
}

ShaderResourceImpl* ShaderHandleTable::resolve(uint32_t localId) const
{
    const auto it = _entries.find(localId);
    if (it == _entries.end()) {
        return nullptr;
    }
    return it->second.impl.get();
}

void ShaderHandleTable::invalidate(uint32_t localId)
{
    auto it = _entries.find(localId);
    if (it != _entries.end()) {
        if (it->second.impl) {
            it->second.impl->destroyGpuResources();
        }
        _entries.erase(it);
    }
}

void ShaderHandleTable::clear()
{
    for (auto& [id, entry] : _entries) {
        (void)id;
        if (entry.impl) {
            entry.impl->destroyGpuResources();
        }
    }
    _entries.clear();
}

void ShaderHandleTable::setMaxEntries(size_t maxEntries)
{
    _maxEntries = maxEntries > 0 ? maxEntries : kDefaultMaxEntries;
}

void ShaderHandleTable::touch(uint32_t localId)
{
    const auto it = _entries.find(localId);
    if (it != _entries.end()) {
        it->second.lruStamp = ++_lruCounter;
    }
}

void ShaderHandleTable::evictIfNeeded()
{
    if (_entries.size() < _maxEntries) {
        return;
    }

    uint32_t oldestId = 0;
    uint64_t oldestStamp = UINT64_MAX;
    for (const auto& [id, entry] : _entries) {
        if (entry.lruStamp < oldestStamp) {
            oldestStamp = entry.lruStamp;
            oldestId = id;
        }
    }
    invalidate(oldestId);
}

} // namespace ayt::shader::detail
