#pragma once
// detail/AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/ShaderDiskCache.h — CompiledShaderProgram disk tier (Phase 4-I)
//
// R-B-01 audit fix (2026-08-26): disk-cache file-path construction must be
// safe against caller-supplied inputs.  The pre-audit `diskCacheFilePath`
// simply concatenated `cacheDirectory + "/" + key`, which meant a caller
// could pass `../../etc/passwd` (or an absolute path) and have the cache
// layer write to an arbitrary filesystem location.
//
// We now expose `sanitizeCacheKey`, an allowlist filter that rejects any
// key containing characters outside `[A-Za-z0-9_-]` (or that is empty
// after sanitisation, or that is one of the `.` / `..` POSIX special
// entries).  `diskCacheFilePath` always runs the input through this
// filter before joining with the base directory; the safe variant is the
// canonical entry point from `ShaderResourcePool`.

#include "AYShader/ShaderProgram.h"

#include <string>

namespace ayt::shader::detail
{

// Returns `true` when `key` is safe to use as the basenames component of a
// cache-file path.  The rules are intentionally strict:
//
//   - non-empty
//   - every byte is in [A-Za-z0-9_-]
//   - does not start with a `.` (POSIX dot-file / `..` escape hatch)
//   - is not exactly `.` or `..`
//
// Any caller-supplied shader name or hash digest that reaches disk-cache
// code must satisfy this contract.  `sha256Hex()` already produces a 64-
// character lowercase hex string so its output passes trivially; the
// filter exists to defend against future keys that incorporate user input
// (e.g. shader source paths) without re-validating here.
bool isSafeCacheKey(const std::string& key);

// Returns the sanitised form of `key`.  Behaviour matches
// `isSafeCacheKey`; if the input would fail the allowlist, the returned
// string is empty.  Callers should treat an empty return as "reject this
// cache entry entirely" rather than silently rewriting the name.
std::string sanitizeCacheKey(const std::string& key);

// Constructs the on-disk path for a CompiledShaderProgram cache file.
// Returns an empty string when either argument is unsafe to write to;
// callers must treat that as "do not persist" rather than treating it
// as a hint to write a fallback path.
std::string diskCacheFilePath(const std::string& cacheDirectory,
                              const std::string& digestHex);

bool loadCompiledProgramFromDisk(const std::string& path,
                                 CompiledShaderProgram& out);
bool saveCompiledProgramToDisk(const std::string& path,
                               const CompiledShaderProgram& prog);

} // namespace ayt::shader::detail
