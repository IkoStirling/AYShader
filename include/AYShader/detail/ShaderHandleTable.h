#pragma once
// detail/AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/ShaderHandleTable.h — opaque handle registry (Phase 4-O)

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ayt::shader
{

class ShaderResourceImpl;
class ShaderResourcePool;

namespace detail
{

class ShaderHandleTable {
public:
    static constexpr size_t kDefaultMaxEntries = 256;

    uint32_t insert(std::unique_ptr<ShaderResourceImpl> impl);
    ShaderResourceImpl* resolve(uint32_t localId) const;
    void invalidate(uint32_t localId);
    void clear();

    void setMaxEntries(size_t maxEntries);
    void touch(uint32_t localId);
    void evictIfNeeded();

private:
    struct Entry {
        std::unique_ptr<ShaderResourceImpl> impl;
        uint64_t lruStamp = 0;
    };

    size_t _maxEntries = kDefaultMaxEntries;
    uint32_t _nextId = 1;
    uint64_t _lruCounter = 0;
    std::unordered_map<uint32_t, Entry> _entries;
};

} // namespace detail
} // namespace ayt::shader
