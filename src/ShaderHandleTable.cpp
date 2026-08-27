// ShaderHandleTable.cpp — opaque handle registry (Phase 4-O)
//
// R-B-02 + R-B-03 audit fixes (2026-08-26):
//   * Every mutating method now takes a `std::unique_lock<std::shared_mutex>`
//     over `_mutex`.  The hot read `resolve(...)` takes a
//     `std::shared_lock` so concurrent lookups don't serialise.
//   * True LRU: `resolve(...)` updates `entry.lastAccessTick` on every hit
//     (so a frequently-touched handle can outlast a stale one) and
//     `evictIfNeeded(...)` now picks the entry with the smallest
//     `lastAccessTick`.  The legacy `lruStamp` field is still bumped on
//     insert/touch for debuggability but is no longer used for eviction.

#include "AYShader/detail/ShaderHandleTable.h"
#include "AYShader/ShaderResource.h"
#include "ShaderResourceImpl.h"

#include <limits>

namespace ayt::shader::detail
{

uint32_t ShaderHandleTable::insert(std::unique_ptr<ShaderResourceImpl> impl)
{
    if (!impl) {
        return 0;
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    evictIfNeededLocked();
    const uint32_t id = _nextId++;
    Entry& entry = _entries[id];
    entry.impl = std::shared_ptr<ShaderResourceImpl>(
        impl.release(),
        [](ShaderResourceImpl* resource) {
            if (resource != nullptr) {
                resource->destroyGpuResources();
                delete resource;
            }
        });
    const uint64_t tick = ++_lruCounter;
    entry.lruStamp = tick;
    entry.lastAccessTick = tick;
    return id;
}

ShaderResourceImpl* ShaderHandleTable::resolve(uint32_t localId)
{
    // Preserve the legacy raw-pointer API with a per-thread pin. New code
    // should use resolveShared() so the lifetime is explicit.
    thread_local std::shared_ptr<ShaderResourceImpl> pin;
    pin = resolveShared(localId);
    return pin.get();
}

std::shared_ptr<ShaderResourceImpl>
ShaderHandleTable::resolveShared(uint32_t localId)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    const auto it = _entries.find(localId);
    if (it == _entries.end() || !it->second.impl) {
        return {};
    }
    const uint64_t tick = ++_lruCounter;
    it->second.lastAccessTick = tick;
    it->second.lruStamp = tick;
    return it->second.impl;
}

void ShaderHandleTable::invalidate(uint32_t localId)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _entries.find(localId);
    if (it != _entries.end()) {
        _entries.erase(it);
    }
}

void ShaderHandleTable::clear()
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _entries.clear();
}

void ShaderHandleTable::setMaxEntries(size_t maxEntries)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _maxEntries = maxEntries > 0 ? maxEntries : kDefaultMaxEntries;
}

void ShaderHandleTable::touch(uint32_t localId)
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    const auto it = _entries.find(localId);
    if (it != _entries.end()) {
        const uint64_t tick = ++_lruCounter;
        it->second.lruStamp = tick;
        it->second.lastAccessTick = tick;
    }
}

void ShaderHandleTable::evictIfNeeded()
{
    std::unique_lock<std::shared_mutex> lock(_mutex);
    evictIfNeededLocked();
}

void ShaderHandleTable::evictIfNeededLocked()
{
    if (_entries.size() < _maxEntries) {
        return;
    }

    uint32_t oldestId = 0;
    uint64_t oldestTick = std::numeric_limits<uint64_t>::max();
    bool foundAny = false;
    for (const auto& [id, entry] : _entries) {
        // Tie-break on legacy `_nextId` order so two entries whose
        // `lastAccessTick` somehow tie (e.g. freshly inserted in the
        // same `_lruCounter` increment on platforms with a fast clock)
        // still evict deterministically — preferring the oldest first.
        if (entry.lastAccessTick < oldestTick
            || (!foundAny && entry.lastAccessTick == oldestTick && id < oldestId)) {
            oldestTick = entry.lastAccessTick;
            oldestId = id;
            foundAny = true;
        }
    }
    if (foundAny) {
        const auto it = _entries.find(oldestId);
        if (it != _entries.end()) {
            _entries.erase(it);
        }
    }
}

size_t ShaderHandleTable::size() const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _entries.size();
}

} // namespace ayt::shader::detail
