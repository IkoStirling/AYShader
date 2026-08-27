#pragma once
// detail/AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/ShaderHandleTable.h — opaque handle registry (Phase 4-O)
//
// R-B-02 + R-B-03 audit fixes (2026-08-26):
//   * The table is now protected by a `std::shared_mutex`.  Resolution also
//     updates the LRU timestamp and therefore takes an exclusive lock;
//     mutations (`insert`,
//     `invalidate`, `clear`, `setMaxEntries`, `touch`, `evictIfNeeded`)
//     take the exclusive `std::unique_lock`.  `touch` is read-write in
//     semantics — it updates the LRU timestamp — so it also goes exclusive.
//
//   * The "LRU" eviction policy now actually corresponds to "least recently
//     accessed": every `resolve` records a fresh `lastAccessTick` via
//     `touch`, and `evictIfNeeded` removes the entry with the oldest
//     `lastAccessTick` (rather than the oldest `_nextId`, which made the
//     pre-audit code FIFO by accident).

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
    // Returns the live impl for `localId`, bumping its LRU timestamp as
    // a side-effect (true-LRU fix, 2026-08-26).  This used to be `const`,
    // but updating the timestamp requires an exclusive-lock upgrade, so
    // we drop the qualifier; the surrounding `ShaderResourcePool::Impl`
    // is non-const anyway and the call site doesn't change.
    ShaderResourceImpl* resolve(uint32_t localId);
    // Retains the implementation for the complete caller operation. This
    // prevents invalidate/clear/LRU eviction from freeing it immediately
    // after the table lock is released.
    std::shared_ptr<ShaderResourceImpl> resolveShared(uint32_t localId);
    void invalidate(uint32_t localId);
    void clear();

    void setMaxEntries(size_t maxEntries);
    void touch(uint32_t localId);
    void evictIfNeeded();

    // Returns the current entry count.  Used by tests to assert eviction
    // behaviour without leaking a non-const iterator to callers.
    size_t size() const;

private:
    void evictIfNeededLocked();

    struct Entry {
        std::shared_ptr<ShaderResourceImpl> impl;
        // Monotonic timestamps recorded every time the entry was last
        // touched (insert / touch / resolve) — used by `evictIfNeeded` to
        // identify the truly oldest still-live handle.  We keep two
        // stamps (legacy `lruStamp` for binary-back-compat, plus
        // `lastAccessTick` which is updated on every resolve) so the
        // table is internally self-describing for future debugging.
        uint64_t lruStamp = 0;
        uint64_t lastAccessTick = 0;
    };

    mutable std::shared_mutex _mutex;
    size_t _maxEntries = kDefaultMaxEntries;
    uint32_t _nextId = 1;
    uint64_t _lruCounter = 0;
    std::unordered_map<uint32_t, Entry> _entries;
};

} // namespace detail
} // namespace ayt::shader
