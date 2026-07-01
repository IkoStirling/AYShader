#pragma once
// AYShaderCache.h - Shader compilation cache

#include "AYShaderProgram.h"
#include "detail/AYShaderProgramLegacy.h"
#include "IAYBackendConverter.h"
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <optional>

namespace ayt::shader
{

struct ShaderContentKey {
    std::string sourcePath;
    std::array<uint8_t, 32> sourceHash;
    Platform platform;

    bool operator==(const ShaderContentKey& other) const {
        return sourcePath == other.sourcePath
            && sourceHash == other.sourceHash
            && platform == other.platform;
    }
};

class ShaderCache {
public:
    ShaderCache() = default;

    void setCacheDirectory(const std::string& path);
    std::string getCacheDirectory() const;

    std::optional<ShaderProgram> find(const ShaderContentKey& key);
    void store(const ShaderContentKey& key, const ShaderProgram& program);

    bool verifyHash(const std::string& sourcePath, const std::array<uint8_t, 32>& expectedHash);

    static std::array<uint8_t, 32> computeHash(const std::string& source);

private:
    std::string _cacheDirectory;
    std::string getCachePath(const ShaderContentKey& key) const;
};

} // namespace ayt::shader
