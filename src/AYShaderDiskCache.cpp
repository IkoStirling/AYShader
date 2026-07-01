// AYShaderDiskCache.cpp — CompiledShaderProgram disk tier (Phase 4-I)
//
// File I/O migrated to AYFoundation/AYIO (ayt::io) per the AYShader
// file-IO migration plan. The on-disk binary format is unchanged; only
// the read/write open/close plumbing moved.

#include "detail/AYShaderDiskCache.h"

#include <AYFile.h>
#include <AYDirectory.h>

#include <cstdint>
#include <cstring>
#include <sstream>

namespace ayt::shader::detail
{

namespace {

constexpr char kMagic[4] = {'A', 'Y', 'S', 'C'};
constexpr uint32_t kVersion = 1;

bool readBytes(std::istream& in, void* dst, size_t size)
{
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount()) == size;
}

bool writeBytes(std::ostream& out, const void* src, size_t size)
{
    out.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(size));
    return out.good();
}

bool readString(std::istream& in, std::string& out)
{
    uint32_t len = 0;
    if (!readBytes(in, &len, sizeof(len))) {
        return false;
    }
    out.resize(len);
    if (len == 0) {
        return true;
    }
    return readBytes(in, out.data(), len);
}

bool writeString(std::ostream& out, const std::string& value)
{
    const uint32_t len = static_cast<uint32_t>(value.size());
    return writeBytes(out, &len, sizeof(len))
        && (value.empty() || writeBytes(out, value.data(), value.size()));
}

bool readBlob(std::istream& in, std::vector<uint8_t>& out)
{
    uint32_t len = 0;
    if (!readBytes(in, &len, sizeof(len))) {
        return false;
    }
    out.resize(len);
    if (len == 0) {
        return true;
    }
    return readBytes(in, out.data(), len);
}

bool writeBlob(std::ostream& out, const std::vector<uint8_t>& value)
{
    const uint32_t len = static_cast<uint32_t>(value.size());
    return writeBytes(out, &len, sizeof(len))
        && (value.empty() || writeBytes(out, value.data(), value.size()));
}

bool readUniformBlockMember(std::istream& in, BGFXUniformBlockMember& member)
{
    uint64_t offset = 0;
    uint64_t size = 0;
    if (!readString(in, member.name)
        || !readString(in, member.type)
        || !readBytes(in, &offset, sizeof(offset))
        || !readBytes(in, &size, sizeof(size))) {
        return false;
    }
    member.offsetBytes = static_cast<size_t>(offset);
    member.sizeBytes = static_cast<size_t>(size);
    return true;
}

bool writeUniformBlockMember(std::ostream& out, const BGFXUniformBlockMember& member)
{
    const uint64_t offset = static_cast<uint64_t>(member.offsetBytes);
    const uint64_t size = static_cast<uint64_t>(member.sizeBytes);
    return writeString(out, member.name)
        && writeString(out, member.type)
        && writeBytes(out, &offset, sizeof(offset))
        && writeBytes(out, &size, sizeof(size));
}

bool readUniformBlock(std::istream& in, BGFXUniformBlock& block)
{
    int32_t binding = -1;
    uint64_t sizeBytes = 0;
    uint32_t memberCount = 0;
    if (!readString(in, block.name)
        || !readBytes(in, &binding, sizeof(binding))
        || !readBytes(in, &sizeBytes, sizeof(sizeBytes))
        || !readBytes(in, &memberCount, sizeof(memberCount))) {
        return false;
    }
    block.binding = binding;
    block.sizeBytes = static_cast<size_t>(sizeBytes);
    block.fieldNames.clear();
    block.members.clear();
    block.members.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount; ++i) {
        BGFXUniformBlockMember member;
        if (!readUniformBlockMember(in, member)) {
            return false;
        }
        block.fieldNames.push_back(member.name);
        block.members.push_back(std::move(member));
    }
    return true;
}

bool writeUniformBlock(std::ostream& out, const BGFXUniformBlock& block)
{
    const int32_t binding = block.binding;
    const uint64_t sizeBytes = static_cast<uint64_t>(block.sizeBytes);
    const uint32_t memberCount = static_cast<uint32_t>(block.members.size());
    if (!writeString(out, block.name)
        || !writeBytes(out, &binding, sizeof(binding))
        || !writeBytes(out, &sizeBytes, sizeof(sizeBytes))
        || !writeBytes(out, &memberCount, sizeof(memberCount))) {
        return false;
    }
    for (const BGFXUniformBlockMember& member : block.members) {
        if (!writeUniformBlockMember(out, member)) {
            return false;
        }
    }
    return true;
}

bool readUniform(std::istream& in, BGFXUniform& uniform)
{
    return readString(in, uniform.name)
        && readString(in, uniform.type)
        && readBytes(in, &uniform.count, sizeof(uniform.count))
        && readString(in, uniform.blockName)
        && readBytes(in, &uniform.blockBinding, sizeof(uniform.blockBinding));
}

bool writeUniform(std::ostream& out, const BGFXUniform& uniform)
{
    return writeString(out, uniform.name)
        && writeString(out, uniform.type)
        && writeBytes(out, &uniform.count, sizeof(uniform.count))
        && writeString(out, uniform.blockName)
        && writeBytes(out, &uniform.blockBinding, sizeof(uniform.blockBinding));
}

bool readTexture(std::istream& in, BGFXTexture& texture)
{
    return readString(in, texture.name)
        && readBytes(in, &texture.binding, sizeof(texture.binding))
        && readString(in, texture.textureType);
}

bool writeTexture(std::ostream& out, const BGFXTexture& texture)
{
    return writeString(out, texture.name)
        && writeBytes(out, &texture.binding, sizeof(texture.binding))
        && writeString(out, texture.textureType);
}

bool readStorageBuffer(std::istream& in, BGFXStorageBuffer& storage)
{
    return readString(in, storage.name)
        && readBytes(in, &storage.binding, sizeof(storage.binding))
        && readString(in, storage.elementType);
}

bool writeStorageBuffer(std::ostream& out, const BGFXStorageBuffer& storage)
{
    return writeString(out, storage.name)
        && writeBytes(out, &storage.binding, sizeof(storage.binding))
        && writeString(out, storage.elementType);
}

template <typename T, bool (*Reader)(std::istream&, T&)>
bool readVectorT(std::istream& in, std::vector<T>& container)
{
    uint32_t count = 0;
    if (!readBytes(in, &count, sizeof(count))) {
        return false;
    }
    container.clear();
    container.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        T item{};
        if (!Reader(in, item)) {
            return false;
        }
        container.push_back(std::move(item));
    }
    return true;
}

template <typename T, bool (*Writer)(std::ostream&, const T&)>
bool writeVectorT(std::ostream& out, const std::vector<T>& container)
{
    const uint32_t count = static_cast<uint32_t>(container.size());
    if (!writeBytes(out, &count, sizeof(count))) {
        return false;
    }
    for (const T& item : container) {
        if (!Writer(out, item)) {
            return false;
        }
    }
    return true;
}

bool ensureParentDirectory(const std::string& filePath)
{
    const size_t slash = filePath.find_last_of("/\\");
    if (slash == std::string::npos) {
        return true;
    }
    const std::string dir = filePath.substr(0, slash);
    if (dir.empty()) {
        return true;
    }
    // AYIO Directory::exists covers both files and directories; for our
    // purposes we only need "is the path resolvable" before we attempt
    // createRecursive, so we collapse the stat/mkdir dance into two calls.
    if (ayt::io::Directory::exists(dir)) {
        return true;
    }
    return ayt::io::Directory::createRecursive(dir);
}

} // namespace

std::string diskCacheFilePath(const std::string& cacheDirectory,
                              const std::string& digestHex)
{
    return cacheDirectory + "/" + digestHex + ".aysc";
}

bool loadCompiledProgramFromDisk(const std::string& path,
                                 CompiledShaderProgram& out)
{
    // Read whole file as bytes via AYIO, then pipe through a stringstream
    // so the existing read* helpers (which take std::istream&) keep working
    // unchanged. The on-disk binary format is byte-for-byte identical to
    // the pre-migration version.
    const std::vector<uint8_t> bytes = ayt::io::File::readAllBytes(path);
    if (bytes.empty()) {
        return false;
    }
    std::stringstream ss;
    ss.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    std::istream& in = ss;

    char magic[4] = {};
    uint32_t version = 0;
    uint8_t success = 0;
    if (!readBytes(in, magic, sizeof(magic))
        || !readBytes(in, &version, sizeof(version))
        || !readBytes(in, &success, sizeof(success))) {
        return false;
    }
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || version != kVersion) {
        return false;
    }

    out = CompiledShaderProgram{};
    out.success = success != 0;

    return readBlob(in, out.vsBin)
        && readBlob(in, out.fsBin)
        && readBlob(in, out.csBin)
        && readVectorT<BGFXUniformBlock, readUniformBlock>(in, out.uniformBlocks)
        && readVectorT<BGFXUniform, readUniform>(in, out.uniforms)
        && readVectorT<BGFXTexture, readTexture>(in, out.textures)
        && readVectorT<BGFXStorageBuffer, readStorageBuffer>(in, out.storageBuffers);
}

bool saveCompiledProgramToDisk(const std::string& path,
                               const CompiledShaderProgram& prog)
{
    if (!prog.success) {
        return false;
    }
    if (!ensureParentDirectory(path)) {
        return false;
    }

    // Serialize to an in-memory buffer first, then atomically write to disk
    // via AYIO. atomicWrite does write-temp-then-rename, which means a crash
    // mid-write leaves the previous good .aysc on disk rather than a
    // truncated corrupted one. Pre-migration used std::ofstream(trunc), which
    // truncated before any bytes were written — strict regression risk.
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    std::ostream& out = ss;

    const uint8_t success = 1;
    if (!writeBytes(out, kMagic, sizeof(kMagic))
        || !writeBytes(out, &kVersion, sizeof(kVersion))
        || !writeBytes(out, &success, sizeof(success))
        || !writeBlob(out, prog.vsBin)
        || !writeBlob(out, prog.fsBin)
        || !writeBlob(out, prog.csBin)
        || !writeVectorT<BGFXUniformBlock, writeUniformBlock>(out, prog.uniformBlocks)
        || !writeVectorT<BGFXUniform, writeUniform>(out, prog.uniforms)
        || !writeVectorT<BGFXTexture, writeTexture>(out, prog.textures)
        || !writeVectorT<BGFXStorageBuffer, writeStorageBuffer>(out, prog.storageBuffers)) {
        return false;
    }

    // Drain the stringstream into a contiguous byte buffer for AYIO.
    const std::string& buf = ss.str();
    return ayt::io::File::atomicWrite(
        path,
        buf.data(),
        buf.size());
}

} // namespace ayt::shader::detail
