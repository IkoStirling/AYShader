// AYShaderResourcePool.cpp - wire-up, compile path, cache, pool lifetime
//
// R-B-02 audit fix (2026-08-26):
//   The pre-audit pool was unsafe under concurrent calls because it
//   mutated the in-memory `cache`, `sourceCache`, `hotReloadWatches`,
//   `lastCompileErrors`, and `stats` containers without any locking.
//   We now hold a `std::shared_mutex` (`_mutex`) for the lifetime of any
//   mutation; observers (`cacheStats`, `lastCompileErrors`) take a
//   `std::shared_lock` so they can run in parallel with each other.
//
//   The actual shader compile is wrapped between two exclusive locks:
//     1. Read+de-dup from the in-memory cache under a shared lock.
//     2. Drop the lock for the slow compile step (shaderc or disk-read).
//     3. Re-take an exclusive lock to insert the result.
//   This keeps the heavy path out of the critical section while still
//   preventing two threads from racing the same key into the cache. See
//   the audit fix log in commit `…` for the race analysis.
//
// R-H-02 lookup-vs-insert race: the lock release between de-dup and
//   re-insert is bridged by `evictStaleCacheEntries` and the
//   cache-by-key result: if another thread won the race, the second
//   arrival observes a non-empty entry and reuses the existing handle
//   rather than inserting a duplicate.

#include "AYShader/ShaderResourcePool.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "ShaderResourceImpl.h"
#include "AYShader/detail/ShaderDigest.h"
#include "AYShader/detail/ShaderDiskCache.h"
#include "AYShader/detail/ShaderFileWatch.h"
#include "AYShader/detail/ShaderHandleEncoding.h"
#include "AYShader/detail/ShaderHandleTable.h"
#include "AYShader/Ir.h"

#include <AYIO/File.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <bgfx/bgfx.h>

namespace ayt::shader
{

namespace {

constexpr int64_t kHotReloadDebounceMs = 100;
// v4 fixes Phoskia binary-operator associativity; unchanged source must not
// reuse binaries generated with the old expression semantics.
constexpr const char* kCompiledShaderCacheSchema = "aybgfx-v4-left-associative";

struct HotReloadWatch {
    std::string sourcePath;
    int64_t lastMtimeMs = 0;
    bool debouncing = false;
    int64_t debounceStartMs = 0;
    std::vector<std::string> cacheKeys;
    std::vector<uint64_t> liveHandles;
};

} // namespace

namespace {

// Hand-authored bgfx .sc shaders bypass Phoskia/AYBGFXConverter, so binding
// metadata must be synthesized here or ForwardOpaquePass never uploads data.
bool uniformAlreadyDeclared(const CompiledShaderProgram& prog, const std::string& name)
{
    for (const BGFXUniform& u : prog.uniforms) {
        if (u.name == name) {
            return true;
        }
    }
    return false;
}

size_t parseBoneArrayCount(const std::string& source)
{
    size_t boneCount = 4;
    const size_t bonesKey = source.find("bones[");
    if (bonesKey != std::string::npos) {
        const size_t start = bonesKey + 6;
        if (start < source.size() && std::isdigit(static_cast<unsigned char>(source[start]))) {
            boneCount = static_cast<size_t>(std::strtoul(source.c_str() + start, nullptr, 10));
        }
    }
    return boneCount > 0 ? boneCount : 4;
}

bool tryInjectBoneUniformArray(const std::string& vertexSc, CompiledShaderProgram& prog)
{
    if (vertexSc.find("uniform mat4 bones") == std::string::npos) {
        return false;
    }
    if (uniformAlreadyDeclared(prog, "bones")) {
        return true;
    }

    const size_t boneCount = parseBoneArrayCount(vertexSc);
    BGFXUniform uniform;
    uniform.name  = "bones";
    uniform.type  = "mat4";
    uniform.count = static_cast<uint8_t>(boneCount > 255 ? 255 : boneCount);
    prog.uniforms.push_back(std::move(uniform));
    std::fprintf(stderr,
                 "[ShaderResourcePool] synthesized bones[] uniform (mat4 x %zu)\n",
                 boneCount);
    return true;
}

bool tryInjectSkeletonUniformBlock(const std::string& vertexSc, CompiledShaderProgram& prog)
{
    if (vertexSc.find("cbuffer Skeleton") == std::string::npos) {
        return false;
    }
    for (const BGFXUniformBlock& existing : prog.uniformBlocks) {
        if (existing.name == "Skeleton") {
            return true;
        }
    }

    const size_t boneCount = parseBoneArrayCount(vertexSc);

    int binding = 0;
    const size_t regPos = vertexSc.find("register(b");
    if (regPos != std::string::npos) {
        binding = std::atoi(vertexSc.c_str() + regPos + 10);
    }

    BGFXUniformBlock block;
    block.name      = "Skeleton";
    block.binding   = binding;
    block.sizeBytes = boneCount * 64u;
    block.fieldNames.push_back("bones");

    BGFXUniformBlockMember member;
    member.name         = "bones";
    member.type         = "mat4";
    member.offsetBytes  = 0;
    member.sizeBytes    = block.sizeBytes;
    block.members.push_back(member);

    prog.uniformBlocks.push_back(std::move(block));
    std::fprintf(stderr,
                 "[ShaderResourcePool] synthesized Skeleton UBO (%zu bytes, %zu bones)\n",
                 member.sizeBytes, boneCount);
    return true;
}

void tryInjectDeclaredUniforms(const std::string& source, CompiledShaderProgram& prog)
{
    size_t searchFrom = 0;
    while (searchFrom < source.size()) {
        const size_t pos = source.find("uniform ", searchFrom);
        if (pos == std::string::npos) {
            break;
        }
        size_t cursor = pos + 8;
        while (cursor < source.size()
               && std::isspace(static_cast<unsigned char>(source[cursor]))) {
            ++cursor;
        }

        size_t typeStart = cursor;
        while (cursor < source.size()
               && (std::isalnum(static_cast<unsigned char>(source[cursor]))
                   || source[cursor] == '_')) {
            ++cursor;
        }
        if (cursor == typeStart) {
            searchFrom = pos + 8;
            continue;
        }
        const std::string type = source.substr(typeStart, cursor - typeStart);

        while (cursor < source.size()
               && std::isspace(static_cast<unsigned char>(source[cursor]))) {
            ++cursor;
        }
        size_t nameStart = cursor;
        while (cursor < source.size()
               && (std::isalnum(static_cast<unsigned char>(source[cursor]))
                   || source[cursor] == '_')) {
            ++cursor;
        }
        if (cursor == nameStart) {
            searchFrom = pos + 8;
            continue;
        }
        const std::string name = source.substr(nameStart, cursor - nameStart);

        if (type == "mat4" && name == "bones") {
            searchFrom = cursor;
            continue;
        }
        if (uniformAlreadyDeclared(prog, name)) {
            searchFrom = cursor;
            continue;
        }

        BGFXUniform uniform;
        uniform.name = name;
        uniform.type = type;
        uniform.count = 1;
        prog.uniforms.push_back(uniform);

        searchFrom = cursor;
    }
}

void tryInjectTextureBindings(const std::string& fragmentSc, CompiledShaderProgram& prog)
{
    auto alreadyHas = [&](const std::string& name) -> bool {
        for (const BGFXTexture& t : prog.textures) {
            if (t.name == name) {
                return true;
            }
        }
        return false;
    };

    // UI path: legacy s_texColor sampler name.
    if (fragmentSc.find("s_texColor") != std::string::npos && !alreadyHas("s_texColor")) {
        BGFXTexture texBinding;
        texBinding.name    = "s_texColor";
        texBinding.binding = 0;
        prog.textures.push_back(texBinding);
    }

    // Engine materials / hand-authored .sc: SAMPLER2D(name, slot);
    size_t searchFrom = 0;
    while (searchFrom < fragmentSc.size()) {
        const size_t pos = fragmentSc.find("SAMPLER2D(", searchFrom);
        if (pos == std::string::npos) {
            break;
        }
        size_t cursor = pos + 10; // after SAMPLER2D(
        while (cursor < fragmentSc.size()
               && std::isspace(static_cast<unsigned char>(fragmentSc[cursor]))) {
            ++cursor;
        }
        const size_t nameStart = cursor;
        while (cursor < fragmentSc.size()
               && (std::isalnum(static_cast<unsigned char>(fragmentSc[cursor]))
                   || fragmentSc[cursor] == '_')) {
            ++cursor;
        }
        if (cursor == nameStart) {
            searchFrom = pos + 10;
            continue;
        }
        const std::string name = fragmentSc.substr(nameStart, cursor - nameStart);
        while (cursor < fragmentSc.size()
               && std::isspace(static_cast<unsigned char>(fragmentSc[cursor]))) {
            ++cursor;
        }
        if (cursor >= fragmentSc.size() || fragmentSc[cursor] != ',') {
            searchFrom = cursor;
            continue;
        }
        ++cursor;
        while (cursor < fragmentSc.size()
               && std::isspace(static_cast<unsigned char>(fragmentSc[cursor]))) {
            ++cursor;
        }
        const size_t slotStart = cursor;
        while (cursor < fragmentSc.size()
               && std::isdigit(static_cast<unsigned char>(fragmentSc[cursor]))) {
            ++cursor;
        }
        if (cursor == slotStart) {
            searchFrom = cursor;
            continue;
        }
        const int slot = std::atoi(fragmentSc.substr(slotStart, cursor - slotStart).c_str());
        if (!alreadyHas(name) && slot >= 0 && slot < 16) {
            BGFXTexture texBinding;
            texBinding.name    = name;
            texBinding.binding = static_cast<uint8_t>(slot);
            prog.textures.push_back(texBinding);
        }
        searchFrom = cursor;
    }
}

} // namespace

namespace {

void mapRendererTypeToPlatformProfile(bgfx::RendererType::Enum type,
                                      std::string& platform,
                                      std::string& profile)
{
    // Match bgfx examples/scripts/shader.mk (TARGET → platform/profile).
    switch (type) {
    case bgfx::RendererType::Direct3D11:
        platform = "windows";
        profile = "s_5_0";
        break;
    case bgfx::RendererType::Direct3D12:
        platform = "windows";
        // bgfx shaderc emits the renderer-neutral shader container with
        // DXBC payloads for both D3D11 and D3D12. The bundled shaderc does
        // not expose an s_6_0 profile; D3D12 accepts its s_5_0 output.
        profile = "s_5_0";
        break;
    case bgfx::RendererType::Metal:
        platform = "osx";
        profile = "metal";
        break;
    case bgfx::RendererType::Vulkan:
        platform = "linux";
        profile = "spirv";
        break;
    case bgfx::RendererType::OpenGL:
        platform = "linux";
        profile = "120";
        break;
    case bgfx::RendererType::OpenGLES:
        platform = "android";
        profile = "100_es";
        break;
    case bgfx::RendererType::Noop:
    default:
        platform = "linux";
        profile = "430";
        break;
    }
}

} // namespace

namespace detail {

std::optional<bgfx::UniformType::Enum> bgfxUniformTypeFromString(const std::string& type)
{
    if (type == "mat3") {
        return bgfx::UniformType::Mat3;
    }
    if (type == "mat4") {
        return bgfx::UniformType::Mat4;
    }
    // bgfx models scalars and small vectors as Vec4 slots.
    if (type == "float" || type == "int" || type == "uint"
        || type == "vec2" || type == "vec3" || type == "vec4"
        || type == "ivec2" || type == "ivec3" || type == "ivec4"
        || type == "uvec2" || type == "uvec3" || type == "uvec4") {
        return bgfx::UniformType::Vec4;
    }
    return std::nullopt;
}

uint16_t uniformElementCount(const std::string& type, uint8_t declaredCount)
{
    if (declaredCount > 0) {
        return declaredCount;
    }
    if (type == "mat4") {
        return 1;
    }
    if (type == "mat3") {
        return 1;
    }
    return 1;
}

bool isValidBgfxShaderBinary(const std::vector<uint8_t>& bytes, char stageTag)
{
    if (bytes.size() < 4) {
        return false;
    }
    const uint8_t* header = bytes.data();
    return header[0] == static_cast<uint8_t>(stageTag)
        && header[1] == static_cast<uint8_t>('S')
        && header[2] == static_cast<uint8_t>('H');
}

bgfx::ShaderHandle createShaderFromBytes(const std::vector<uint8_t>& bytes, char stageTag,
                                         std::vector<std::string>& errors)
{
    if (bytes.empty()) {
        errors.push_back("shader bytes empty");
        return BGFX_INVALID_HANDLE;
    }
    if (!isValidBgfxShaderBinary(bytes, stageTag)) {
        errors.push_back(std::string("invalid bgfx shader header for stage '")
                         + stageTag + "' (size=" + std::to_string(bytes.size())
                         + ", magic=" + std::to_string(bytes[0]) + std::to_string(bytes[1])
                         + std::to_string(bytes[2]) + std::to_string(bytes[3])
                         + "); check shaderc/bgfx version match");
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size()));
    return bgfx::createShader(mem);
}

BindingId allocateBinding(BindingId& nextId, ShaderResourceImpl& impl, BindingEntry entry)
{
    entry.id = nextId++;
    impl.bindingsById.emplace(entry.id, entry);
    return entry.id;
}

bool buildBindingTable(ShaderResourceImpl& impl, const CompiledShaderProgram& prog,
                       std::vector<std::string>& errors)
{
    BindingId nextId = 1;

    for (const BGFXUniformBlock& block : prog.uniformBlocks) {
        BindingEntry entry;
        entry.kind = BindingKind::UniformBlock;
        entry.name = block.name;
        entry.uniformBlockSizeBytes = block.sizeBytes;

        for (const BGFXUniformBlockMember& member : block.members) {
            entry.uniformBlockFieldOffsets.emplace(member.name, member.offsetBytes);
            entry.uniformBlockFieldSizes.emplace(member.name, member.sizeBytes);
        }

        const size_t blockBytes = block.sizeBytes > 0 ? block.sizeBytes : 16u;
        const uint16_t numVec4 = static_cast<uint16_t>((blockBytes + 15u) / 16u);

        // bgfx skinning: mat4[] inside a cbuffer binds by member name (bones),
        // not the cbuffer label (Skeleton). Vec4 slots are for std140 scalars.
        if (block.members.size() == 1
            && block.members[0].type == "mat4"
            && block.members[0].sizeBytes >= 64u) {
            const uint16_t mat4Count =
                static_cast<uint16_t>(block.members[0].sizeBytes / 64u);
            entry.uniformHandle = bgfx::createUniform(
                block.members[0].name.c_str(),
                bgfx::UniformType::Mat4,
                mat4Count);
            entry.uniformSubmitCount = mat4Count;
            entry.uniformElementSizeBytes = 64u;
        } else {
            entry.uniformHandle = bgfx::createUniform(
                block.name.c_str(), bgfx::UniformType::Vec4, numVec4);
            entry.uniformSubmitCount = numVec4;
            entry.uniformElementSizeBytes = 16u;
        }

        const BindingId id = allocateBinding(nextId, impl, entry);
        impl.uniformBlockBindings.emplace(block.name, id);
    }

    for (const BGFXStorageBuffer& storage : prog.storageBuffers) {
        BindingEntry entry;
        entry.kind = BindingKind::StorageBuffer;
        entry.name = storage.name;
        const BindingId id = allocateBinding(nextId, impl, entry);
        impl.storageBufferBindings.emplace(storage.name, id);
    }

    for (const BGFXUniform& uniform : prog.uniforms) {
        if (!uniform.blockName.empty()) {
            continue;
        }

        const auto bgfxType = bgfxUniformTypeFromString(uniform.type);
        if (!bgfxType.has_value()) {
            errors.push_back("ShaderResource wire-up: unknown uniform type '" + uniform.type
                             + "' for '" + uniform.name + "'");
            return false;
        }

        BindingEntry entry;
        entry.kind = BindingKind::Uniform;
        entry.name = uniform.name;
        const uint16_t count = uniformElementCount(uniform.type, uniform.count);
        entry.uniformSubmitCount = count;
        entry.uniformElementSizeBytes = *bgfxType == bgfx::UniformType::Mat4
            ? 64u
            : (*bgfxType == bgfx::UniformType::Mat3 ? 36u : 16u);
        entry.uniformHandle = bgfx::createUniform(
            uniform.name.c_str(),
            *bgfxType,
            count);

        const BindingId id = allocateBinding(nextId, impl, entry);
        impl.uniformBindings.emplace(uniform.name, id);
    }

    for (const BGFXTexture& texture : prog.textures) {
        BindingEntry entry;
        entry.kind = BindingKind::Texture;
        entry.name = texture.name;
        entry.textureBinding = texture.binding;
        const std::string backendName =
            detail::bgfxTextureSymbolName(texture.name);
        entry.uniformHandle = bgfx::createUniform(
            backendName.c_str(), bgfx::UniformType::Sampler);

        const BindingId id = allocateBinding(nextId, impl, entry);
        impl.textureBindings.emplace(texture.name, id);
    }

    return true;
}

bool wireUpProgram(ShaderResourceImpl& impl, const CompiledShaderProgram& prog,
                   std::vector<std::string>& errors)
{
    const bool hasMaterial = !prog.vsBin.empty() && !prog.fsBin.empty();
    const bool hasCompute = !prog.csBin.empty();

    if (!hasMaterial && !hasCompute) {
        errors.push_back("ShaderResource wire-up: no shader stage bytes present");
        return false;
    }

    if (hasMaterial) {
        impl.vertexShader = createShaderFromBytes(prog.vsBin, 'V', errors);
        impl.fragmentShader = createShaderFromBytes(prog.fsBin, 'F', errors);
        if (!bgfx::isValid(impl.vertexShader) || !bgfx::isValid(impl.fragmentShader)) {
            errors.push_back("ShaderResource wire-up: bgfx::createShader failed for vs/fs");
            return false;
        }
        impl.programHandle = bgfx::createProgram(
            impl.vertexShader, impl.fragmentShader, true);
        if (!bgfx::isValid(impl.programHandle)) {
            if (bgfx::isValid(impl.vertexShader)) {
                bgfx::destroy(impl.vertexShader);
            }
            if (bgfx::isValid(impl.fragmentShader)) {
                bgfx::destroy(impl.fragmentShader);
            }
            impl.vertexShader = BGFX_INVALID_HANDLE;
            impl.fragmentShader = BGFX_INVALID_HANDLE;
            errors.push_back("ShaderResource wire-up: bgfx::createProgram failed");
            return false;
        }
        impl.vertexShader = BGFX_INVALID_HANDLE;
        impl.fragmentShader = BGFX_INVALID_HANDLE;
    } else {
        impl.computeShader = createShaderFromBytes(prog.csBin, 'C', errors);
        if (!bgfx::isValid(impl.computeShader)) {
            errors.push_back("ShaderResource wire-up: bgfx::createShader failed for cs");
            return false;
        }
        impl.programHandle = bgfx::createProgram(impl.computeShader, true);
        if (!bgfx::isValid(impl.programHandle)) {
            if (bgfx::isValid(impl.computeShader)) {
                bgfx::destroy(impl.computeShader);
            }
            impl.computeShader = BGFX_INVALID_HANDLE;
            errors.push_back("ShaderResource wire-up: bgfx::createProgram failed");
            return false;
        }
        impl.computeShader = BGFX_INVALID_HANDLE;
    }

    if (!bgfx::isValid(impl.programHandle)) {
        errors.push_back("ShaderResource wire-up: bgfx::createProgram failed");
        return false;
    }

    // R-H-01 audit fix (2026-08-26): a binding-table failure (partial
    // uniform/sampler creation, unknown type, ... previously leaked the
    // successfully-created program handle AND every uniform handle
    // already inserted into `impl.bindingsById`.  We now destroy the
    // partial binding table AND the new program on any error reported by
    // `buildBindingTable`.  `destroyGpuResources` is idempotent and
    // null-safe against the `_entries` we never populated.
    if (!buildBindingTable(impl, prog, errors)) {
        impl.destroyGpuResources();
        return false;
    }
    return true;
}

} // namespace detail

void ShaderResourceImpl::destroyGpuResources()
{
    if (bgfx::isValid(programHandle)) {
        bgfx::destroy(programHandle);
        programHandle = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(vertexShader)) {
        bgfx::destroy(vertexShader);
        vertexShader = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(fragmentShader)) {
        bgfx::destroy(fragmentShader);
        fragmentShader = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(computeShader)) {
        bgfx::destroy(computeShader);
        computeShader = BGFX_INVALID_HANDLE;
    }

    for (auto& [id, entry] : bindingsById) {
        (void)id;
        if (bgfx::isValid(entry.uniformHandle)) {
            bgfx::destroy(entry.uniformHandle);
            entry.uniformHandle = BGFX_INVALID_HANDLE;
        }
    }
}

struct ShaderResourcePool::Impl {
    static std::unordered_map<uint32_t, Impl*>& poolRegistry()
    {
        // Intentionally leaked: survives static destruction of pools at process exit.
        static std::unordered_map<uint32_t, Impl*>* registry =
            new std::unordered_map<uint32_t, Impl*>();
        return *registry;
    }

    static std::mutex& poolRegistryMutex()
    {
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    static void registerPool(uint32_t serial, Impl* impl)
    {
        std::lock_guard<std::mutex> lock(poolRegistryMutex());
        poolRegistry()[serial] = impl;
    }

    static void unregisterPool(uint32_t serial)
    {
        std::lock_guard<std::mutex> lock(poolRegistryMutex());
        poolRegistry().erase(serial);
    }

    static std::shared_ptr<ShaderResourceImpl> retainPoolHandle(uint32_t serial,
                                                                uint32_t localId)
    {
        // Keep the registry lock until the handle-table reference has been
        // retained, preventing unregister from racing the Impl access.
        std::lock_guard<std::mutex> lock(poolRegistryMutex());
        const auto it = poolRegistry().find(serial);
        if (it == poolRegistry().end() || it->second == nullptr) {
            return {};
        }
        return it->second->handles.resolveShared(localId);
    }

    static std::atomic<uint32_t> s_nextPoolSerial;

    uint32_t poolSerial = 0;
    std::string shadercPath;
    std::string platform =
#if defined(_WIN32)
        "windows"
#elif defined(__APPLE__)
        "osx"
#else
        "linux"
#endif
        ;
    std::string profile =
#if defined(_WIN32)
        "s_5_0"
#else
        "430"
#endif
        ;
    bool platformExplicit = false;
    bool profileExplicit = false;
    bool autoProbe = true;
    bool testRendererBound = false;
    uint8_t testRendererType = 0;
    std::string testPlatform;
    std::string testProfile;
    std::vector<std::string> bgfxIncludeDirs;
    std::string cacheDirectory;
    bool hotReloadEnabled = false;
    ShaderCapability requiredCaps =
        ShaderCapability::VertexFragment | ShaderCapability::Ubo
        | ShaderCapability::Ssbo | ShaderCapability::Compute;

    detail::ShaderHandleTable handles;
    std::unordered_map<std::string, uint64_t> cache;
    std::unordered_map<std::string, std::shared_ptr<const phoskia::ir::IRProgram>> sourceCache;
    std::unordered_map<std::string, HotReloadWatch> hotReloadWatches;
    CacheStats stats;
    std::vector<std::string> lastCompileErrors;
    phoskia::CompileOptions defaultCompileOpts;

    // R-B-02 audit fix (2026-08-26): protecting the mutable state of the
    // pool.  Read-only observers (`cacheStats`, `lastCompileErrors`)
    // acquire a `std::shared_lock`.  Mutating methods (`acquire`,
    // `release`, `evictStaleCacheEntries`, `registerHotReloadWatch`,
    // `invalidateHotReloadWatch`, `pollHotReloadWatches`, ...) take a
    // `std::unique_lock`.  Mutex ordering: the pool mutex is always
    // acquired BEFORE any call into `ShaderHandleTable` (which has its
    // own internal shared_mutex).
    mutable std::shared_mutex mutex;

    Impl()
    {
        poolSerial = s_nextPoolSerial.fetch_add(1);
        registerPool(poolSerial, this);
    }

    ~Impl()
    {
        unregisterPool(poolSerial);
    }

    void ensurePlatformProfileResolved()
    {
        if (testRendererBound) {
            platform = testPlatform;
            profile = testProfile;
            return;
        }
        if (platformExplicit && profileExplicit) {
            return;
        }
        if (!autoProbe) {
            return;
        }
        const bgfx::Caps* caps = bgfx::getCaps();
        if (caps != nullptr) {
            std::string probedPlatform = platform;
            std::string probedProfile = profile;
            mapRendererTypeToPlatformProfile(caps->rendererType, probedPlatform, probedProfile);
            if (!platformExplicit) {
                platform = probedPlatform;
            }
            if (!profileExplicit) {
                profile = probedProfile;
            }
            return;
        }

        // bgfx::getCaps() unavailable — avoid linux/430 defaults on Windows hosts.
        if (!platformExplicit || !profileExplicit) {
#if defined(_WIN32)
            if (!platformExplicit) {
                platform = "windows";
            }
            if (!profileExplicit) {
                profile = "s_5_0";
            }
#elif defined(__APPLE__)
            if (!platformExplicit) {
                platform = "osx";
            }
            if (!profileExplicit) {
                profile = "metal";
            }
#else
            if (!platformExplicit) {
                platform = "linux";
            }
            if (!profileExplicit) {
                profile = "spirv";
            }
#endif
        }
    }

    uint64_t makeHandle(std::unique_ptr<ShaderResourceImpl> impl)
    {
        const uint32_t localId = handles.insert(std::move(impl));
        if (localId == 0) {
            return 0;
        }
        return detail::makeShaderHandle(poolSerial, localId);
    }

    std::shared_ptr<ShaderResourceImpl> resolveLocalShared(uint32_t localId)
    {
        return handles.resolveShared(localId);
    }

    void eraseCacheKey(const std::string& key)
    {
        cache.erase(key);
        if (!cacheDirectory.empty()) {
            const std::string diskPath = detail::diskCacheFilePath(cacheDirectory, key);
            // AYIO File::remove is a thin wrapper that maps std::remove's
            // error semantics to a bool (true == success or already absent).
            ayt::io::File::remove(diskPath);
        }
    }

    void removeHandleFromCache(uint64_t handle)
    {
        for (auto it = cache.begin(); it != cache.end(); ) {
            if (it->second == handle) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void registerHotReloadWatch(const std::string& path,
                                const std::string& cacheKey,
                                uint64_t handle)
    {
        if (!hotReloadEnabled || handle == 0) {
            return;
        }

        const std::string normalized = detail::normalizeSourcePath(path);
        HotReloadWatch& watch = hotReloadWatches[normalized];
        watch.sourcePath = path;
        if (const std::optional<int64_t> mtime = detail::fileMtimeMs(path)) {
            watch.lastMtimeMs = *mtime;
        }
        watch.debouncing = false;

        if (std::find(watch.cacheKeys.begin(), watch.cacheKeys.end(), cacheKey)
            == watch.cacheKeys.end()) {
            watch.cacheKeys.push_back(cacheKey);
        }
        watch.liveHandles.push_back(handle);
    }

    void invalidateHotReloadWatch(HotReloadWatch& watch)
    {
        for (const std::string& key : watch.cacheKeys) {
            eraseCacheKey(key);
        }
        for (const uint64_t handle : watch.liveHandles) {
            const uint32_t localId = detail::shaderHandleLocalId(handle);
            handles.invalidate(localId);
            removeHandleFromCache(handle);
        }
        watch.liveHandles.clear();
        watch.cacheKeys.clear();
        watch.debouncing = false;
        if (const std::optional<int64_t> mtime = detail::fileMtimeMs(watch.sourcePath)) {
            watch.lastMtimeMs = *mtime;
        }
    }

    void pollHotReloadWatches()
    {
        if (!hotReloadEnabled) {
            return;
        }

        const int64_t nowMs = detail::steadyClockMs();
        // R-H-04 audit fix (2026-08-26): the pre-audit poll kept watching
        // a source path after its handles were invalidated, leaving the
        // entry alive forever and re-running `fileMtimeMs` on a missing
        // file every frame.  We now erase any entry whose file vanished
        // and which has no live handles.  For entries whose file still
        // exists, we invalidate-and-erase ONLY when we successfully
        // process an mtime change; otherwise we leave the watch in place
        // so a later edit can re-fire.
        std::vector<std::string> toErase;
        for (auto& [normalizedPath, watch] : hotReloadWatches) {
            (void)normalizedPath;
            const std::optional<int64_t> mtime = detail::fileMtimeMs(watch.sourcePath);
            if (!mtime.has_value()) {
                // Source file disappeared or is unreadable.  Drop the
                // watch entirely so it doesn't accumulate forever.
                toErase.push_back(normalizedPath);
                continue;
            }
            if (*mtime == watch.lastMtimeMs) {
                watch.debouncing = false;
                continue;
            }
            if (!watch.debouncing) {
                watch.debouncing = true;
                watch.debounceStartMs = nowMs;
                continue;
            }
            if (nowMs - watch.debounceStartMs >= kHotReloadDebounceMs) {
                invalidateHotReloadWatch(watch);
                // After invalidation the watch has no live handles / keys
                // — schedule it for removal from the map so subsequent
                // edits to the same source path rebuild the watch from
                // scratch on the next compileFromFile call.
                toErase.push_back(normalizedPath);
            }
        }
        for (const std::string& key : toErase) {
            hotReloadWatches.erase(key);
        }
    }

    BGFXCompileOptions engineBgfxOpts(const phoskia::CompileOptions& opts) const
    {
        BGFXCompileOptions bgfxOpts;
        bgfxOpts.shadercPath = shadercPath;
        bgfxOpts.platform = platform;
        bgfxOpts.profile = profile;
        bgfxOpts.includeDirs = bgfxIncludeDirs;
        bgfxOpts.defines = opts.defines;
        bgfxOpts.keepSources = opts.keepSources;
        bgfxOpts.dumpIntermediate = opts.dumpIntermediate;
        bgfxOpts.dumpDir = opts.dumpDir;
        return bgfxOpts;
    }

    std::string makeSourceCacheKey(const std::string& src) const
    {
        return detail::sha256Hex(src);
    }

    std::string makeCacheKeyMaterial(const std::string& keyOverride,
                                     const std::string& src,
                                     const phoskia::CompileOptions& opts) const
    {
        std::ostringstream oss;
        oss << kCompiledShaderCacheSchema << '|';
        if (!keyOverride.empty()) {
            oss << keyOverride << '|';
        }
        oss << platform << '|' << profile << '|' << shadercPath << '|';
        for (const std::string& dir : bgfxIncludeDirs) {
            oss << dir << ';';
        }
        oss << '|' << static_cast<uint32_t>(requiredCaps) << '|';
        for (const std::string& define : opts.defines) {
            oss << define << ';';
        }
        oss << '|' << opts.keepSources
            << opts.dumpIntermediate
            << '|' << src;
        return oss.str();
    }

    std::string makeCacheKey(const std::string& keyOverride,
                             const std::string& src,
                             const phoskia::CompileOptions& opts) const
    {
        return detail::sha256Hex(makeCacheKeyMaterial(keyOverride, src, opts));
    }

    void evictStaleCacheEntries()
    {
        for (auto it = cache.begin(); it != cache.end(); ) {
            const std::shared_ptr<ShaderResourceImpl> impl =
                resolveLocalShared(detail::shaderHandleLocalId(it->second));
            if (impl == nullptr || !bgfx::isValid(impl->programHandle)) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void shutdownAll()
    {
        handles.clear();
        cache.clear();
        sourceCache.clear();
        hotReloadWatches.clear();
    }
};

ShaderResourcePool::ShaderResourcePool()
    : _impl(std::make_unique<Impl>())
{
}

ShaderResourcePool::~ShaderResourcePool()
{
    shutdown();
}

ShaderResourcePool::ShaderResourcePool(ShaderResourcePool&&) noexcept = default;

ShaderResourcePool& ShaderResourcePool::operator=(ShaderResourcePool&&) noexcept = default;

// ---------------------------------------------------------------------------
// R-B-02 audit fix: every entry point that mutates `_impl` state now takes
// either a shared or exclusive `_impl->mutex` lock.  Configuration setters
// (`setShadercExecutable`, `setPlatform`, ...) take exclusive because they
// mutate `shadercPath` / `platform` / `profile` / etc. mid-flight; observers
// (`cacheStats`, `lastCompileErrors`) take shared.  `Mutex ordering: never
// acquire any other non-recursive lock while holding the pool mutex,
// because `acquire(...)` releases it during the slow compile step and we
// don't want to expose inversions.
// ---------------------------------------------------------------------------

std::unique_lock<std::shared_mutex>
ShaderResourcePool::lockOrCreateImplExclusive()
{
    if (_impl == nullptr) {
        _impl = std::make_unique<Impl>();
    }
    return std::unique_lock<std::shared_mutex>(_impl->mutex);
}

void ShaderResourcePool::setShadercExecutable(const std::string& path)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->shadercPath = path;
}

void ShaderResourcePool::setBgfxIncludeDirs(const std::vector<std::string>& dirs)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->bgfxIncludeDirs = dirs;
}

void ShaderResourcePool::setPlatform(const std::string& platform)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->platform = platform;
    _impl->platformExplicit = true;
}

void ShaderResourcePool::setGLSLProfile(const std::string& profile)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->profile = profile;
    _impl->profileExplicit = true;
}

void ShaderResourcePool::setCacheDirectory(const std::string& path)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->cacheDirectory = path;
}

void ShaderResourcePool::setHotReloadEnabled(bool enabled)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->hotReloadEnabled = enabled;
}

void ShaderResourcePool::setIntermediateDumpDirectory(const std::string& path)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->defaultCompileOpts.dumpDir = path;
    _impl->defaultCompileOpts.dumpIntermediate = !path.empty();
    _impl->defaultCompileOpts.keepSources = !path.empty();
}

void ShaderResourcePool::require(ShaderCapability capability)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->requiredCaps = capability;
}

void ShaderResourcePool::setAutoProbeFromRendererType(bool enabled)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->autoProbe = enabled;
}

void ShaderResourcePool::resolvePlatformFromRenderer()
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->ensurePlatformProfileResolved();
    std::fprintf(stderr, "[ShaderResourcePool] compile target: platform=%s profile=%s\n",
                 _impl->platform.c_str(), _impl->profile.c_str());
}

void ShaderResourcePool::bindRendererTypeForTests(uint8_t bgfxRendererType,
                                                  const std::string& platform,
                                                  const std::string& profile)
{
    auto implLock = lockOrCreateImplExclusive();
    _impl->testRendererBound = true;
    _impl->testRendererType = bgfxRendererType;
    _impl->testPlatform = platform;
    _impl->testProfile = profile;
}

CacheStats ShaderResourcePool::cacheStats() const
{
    if (!_impl) {
        return CacheStats{};
    }
    // Shared lock: readers can run in parallel with other observers.
    std::shared_lock<std::shared_mutex> implLock(_impl->mutex);
    return _impl->stats;
}

const std::vector<std::string>& ShaderResourcePool::lastCompileErrors() const
{
    static const std::vector<std::string> kEmpty;
    thread_local std::vector<std::string> snapshot;
    if (!_impl) {
        return kEmpty;
    }
    // R-B-02 audit fix: take a SHARED lock so observers don't serialise
    // against each other.  The returned reference is valid only as long
    // as the returned state — concurrent `acquire`/`release` may
    // reallocate the underlying vector between this call and the
    // caller's next use of the reference. Return a per-thread snapshot so
    // concurrent compiles cannot reallocate storage behind the caller.
    std::shared_lock<std::shared_mutex> implLock(_impl->mutex);
    snapshot = _impl->lastCompileErrors;
    return snapshot;
}

ShaderResourceImpl* ShaderResourcePool::resolveHandle(uint64_t handle)
{
    thread_local std::shared_ptr<ShaderResourceImpl> pin;
    pin = retainHandle(handle);
    return pin.get();
}

std::shared_ptr<ShaderResourceImpl>
ShaderResourcePool::retainHandle(uint64_t handle)
{
    if (handle == 0) {
        return {};
    }
    return Impl::retainPoolHandle(detail::shaderHandlePoolSerial(handle),
                                  detail::shaderHandleLocalId(handle));
}

ShaderResource ShaderResourcePool::compile(const std::string& src)
{
    return acquire(src, phoskia::CompileOptions{}, "");
}

ShaderResource ShaderResourcePool::compile(const std::string& src,
                                           const phoskia::CompileOptions& opts)
{
    return acquire(src, opts, "");
}

ShaderResource ShaderResourcePool::compileFromFile(const std::string& path)
{
    return compileFromFile(path, phoskia::CompileOptions{});
}

ShaderResource ShaderResourcePool::compileFromFile(const std::string& path,
                                                   const phoskia::CompileOptions& opts)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    // R-H-02 audit fix: read the file OUTSIDE the lock so the slow I/O
    // doesn't extend our critical section.  We then re-take the lock to
    // register the hot-reload watch after the result comes back.
    std::string src;
    if (!detail::readTextFile(path, src)) {
        return ShaderResource{};
    }

    ShaderResource res = acquire(src, opts, "");
    if (res.isValid()) {
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        if (_impl->hotReloadEnabled) {
            const std::string key = _impl->makeCacheKey("", src, opts);
            _impl->registerHotReloadWatch(path, key, res.id());
        }
    }
    return res;
}

void ShaderResourcePool::pollHotReload()
{
    if (!_impl) {
        return;
    }
    std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
    _impl->pollHotReloadWatches();
}

ShaderResource ShaderResourcePool::acquire(const std::string& src,
                                           const std::string& cacheKey)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    // R-B-02: snapshot `defaultCompileOpts` under a shared lock so a
    // concurrent `setIntermediateDumpDirectory(...)` doesn't tear the
    // read.  The full lock lives on the inside of `acquire(src, opts, ...)`.
    phoskia::CompileOptions defaultsCopy;
    {
        std::shared_lock<std::shared_mutex> implLock(_impl->mutex);
        defaultsCopy = _impl->defaultCompileOpts;
    }
    return acquire(src, defaultsCopy, cacheKey);
}

ShaderResource ShaderResourcePool::acquire(const std::string& src,
                                           const phoskia::CompileOptions& opts,
                                           const std::string& cacheKey)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

    // R-B-02 + R-H-02 audit fixes (2026-08-26): the cache lookup path is
    // wrapped in a SHARED lock so concurrent acquires for DISTINCT keys
    // don't serialise.  The slow compile step (shaderc or disk I/O) is
    // done OUTSIDE the lock; we only re-take an EXCLUSIVE lock around
    // the final cache insertion to prevent two threads both observing a
    // miss on the same key from racing to insert duplicate handles.
    std::string key;
    std::string sourceKey;
    CompiledShaderProgram prog;
    bool diskLoadOk = false;
    std::string diskLoadPath;
    {
        // Platform probing, stale-entry eviction, statistics and cache
        // mutation all require exclusive ownership. Disk I/O is performed
        // after this short critical section.
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        _impl->ensurePlatformProfileResolved();
        _impl->evictStaleCacheEntries();
        key = _impl->makeCacheKey(cacheKey, src, opts);
        sourceKey = _impl->makeSourceCacheKey(src);

        if (const auto it = _impl->cache.find(key); it != _impl->cache.end()) {
            ShaderResource cached(it->second);
            if (cached.isValid()) {
                ++_impl->stats.binaryHits;
                if (_impl->sourceCache.count(sourceKey) != 0) {
                    ++_impl->stats.sourceHits;
                }
                return cached;
            }
        }

        // R-B-01 audit fix: `diskCacheFilePath` returns an empty string
        // when the key isn't safe.  The load helper already refuses
        // empty paths, so the empty-string short-circuit is just a
        // defence-in-depth check.
        if (!_impl->cacheDirectory.empty()) {
            diskLoadPath = detail::diskCacheFilePath(_impl->cacheDirectory, key);
        }
    }
    if (!diskLoadPath.empty()
        && detail::loadCompiledProgramFromDisk(diskLoadPath, prog)
        && prog.success) {
        diskLoadOk = true;
    }

    if (!diskLoadOk) {
        // Slow compile step — no lock held.  We increment
        // `binaryMisses` later, just before we attempt the cache insert
        // under the exclusive lock.  Counting here would require
        // re-acquiring the lock solely for stats; we keep the critical
        // section tight and accept the very minor accounting skew in
        // exchange.  See R-B-02 audit notes.
        // Slow compile step — no lock held.  Generate the IR.
        phoskia::Compiler compiler;
        std::shared_ptr<const phoskia::ir::IRProgram> cachedIr;
        {
            std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
            const auto irIt = _impl->sourceCache.find(sourceKey);
            if (irIt != _impl->sourceCache.end()) {
                cachedIr = irIt->second;
                ++_impl->stats.sourceHits;
            }
        }
        if (!cachedIr) {
            {
                std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
                ++_impl->stats.sourceMisses;
            }
            phoskia::ir::IRProgram generated;
            std::vector<std::string> irErrors;
            if (!compiler.generateIr(src, opts, generated, irErrors)) {
                prog.success = false;
                prog.errors = std::move(irErrors);
                std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
                _impl->lastCompileErrors = prog.errors;
                return ShaderResource{};
            }
            cachedIr = std::make_shared<const phoskia::ir::IRProgram>(std::move(generated));
            {
                std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
                _impl->sourceCache[sourceKey] = cachedIr;
            }
        }

        shader::AYBGFXConverter converter;
        BGFXCompileOptions bgfxOpts;
        {
            std::shared_lock<std::shared_mutex> implLock(_impl->mutex);
            bgfxOpts = _impl->engineBgfxOpts(opts);
        }
        converter.compileToBinary(*cachedIr, bgfxOpts, prog);
    }

    for (const std::string& warning : prog.warnings) {
        std::fprintf(stderr, "[ShaderResourcePool] %s\n", warning.c_str());
    }
    if (opts.dumpIntermediate && !opts.dumpDir.empty() && prog.success) {
        std::fprintf(stderr, "[ShaderResourcePool] dumped intermediate .sc to %s\n",
                     opts.dumpDir.c_str());
    }

    // Wire up without holding the pool mutex. acquire(prog) maintains its
    // own short critical sections; calling it while holding implLock would
    // recursively acquire the non-recursive shared_mutex and deadlock.
    ShaderResource res = acquire(prog);
    if (!res.isValid()) {
        if (!prog.errors.empty()) {
            std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
            _impl->lastCompileErrors = prog.errors;
        }
        return res;
    }

    // Persist only after GPU wire-up succeeded, so an invalid binary cannot
    // become a permanently reloaded disk-cache entry.
    std::string diskPath;
    {
        std::shared_lock<std::shared_mutex> implLock(_impl->mutex);
        diskPath = detail::diskCacheFilePath(_impl->cacheDirectory, key);
    }
    if (!diskPath.empty()) {
        detail::saveCompiledProgramToDisk(diskPath, prog);
    }

    // Another thread may have inserted the same key while this thread was
    // compiling/wiring. Prefer its handle and destroy our duplicate.
    std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
    const auto it = _impl->cache.find(key);
    if (it != _impl->cache.end()) {
        ShaderResource existing(it->second);
        if (existing.isValid()) {
            _impl->handles.invalidate(detail::shaderHandleLocalId(res.id()));
            res.reset();
            ++_impl->stats.binaryHits;
            return existing;
        }
    }
    ++_impl->stats.binaryMisses;
    _impl->cache[key] = res.id();
    _impl->lastCompileErrors.clear();
    return res;
}

void ShaderResourcePool::release(ShaderResource& res)
{
    if (res.id() == 0 || !_impl) {
        res.reset();
        return;
    }

    std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
    const uint32_t localId = detail::shaderHandleLocalId(res.id());
    _impl->handles.invalidate(localId);      // acquires handle table mutex (acceptable — nested-exclusive)
    _impl->removeHandleFromCache(res.id());
    res.reset();
}

ShaderResource ShaderResourcePool::acquireFromBgfxSc(const std::string& vertexSc,
                                                     const std::string& fragmentSc,
                                                     const std::string& varyingDefSc,
                                                     const std::string& cacheKey)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

    std::string key;
    {
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        _impl->ensurePlatformProfileResolved();
        const std::string keyMaterial = _impl->makeCacheKeyMaterial(
            cacheKey.empty() ? "bgfx_sc" : cacheKey,
            vertexSc + "\n---\n" + fragmentSc + "\n---\n" + varyingDefSc,
            phoskia::CompileOptions{});
        key = detail::sha256Hex(keyMaterial);

        const auto cacheIt = _impl->cache.find(key);
        if (cacheIt != _impl->cache.end()) {
            const std::shared_ptr<ShaderResourceImpl> cached =
                _impl->resolveLocalShared(detail::shaderHandleLocalId(cacheIt->second));
            if (cached != nullptr && bgfx::isValid(cached->programHandle)) {
                ++_impl->stats.binaryHits;
                return ShaderResource(cacheIt->second);
            }
            _impl->cache.erase(key);
        }
    }

    BGFXCompileOptions opts;
    {
        std::shared_lock<std::shared_mutex> implLock(_impl->mutex);
        opts = _impl->engineBgfxOpts(phoskia::CompileOptions{});
    }

    std::optional<AYShadercDriver> driver;
    try {
        // A pool-local executable must not first require a process default.
        // Construct inside the guard so missing configuration fails closed.
        if (opts.shadercPath.empty()) driver.emplace();
        else driver.emplace(opts.shadercPath);
    } catch (const std::exception& e) {
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        _impl->lastCompileErrors = {std::string("shaderc unavailable: ") + e.what()};
        return ShaderResource{};
    }

    auto compileStage = [&](const char* stage, const std::string& source,
                            std::vector<uint8_t>& out) -> bool {
        ShaderCompileRequest req;
        req.scSource           = source;
        req.stage              = stage;
        req.varyingdefSource   = varyingDefSc;
        req.platform           = opts.platform;
        req.profile            = opts.profile;
        req.includeDirs        = opts.includeDirs;
        req.outputName         = std::string("bgfx_sc_") + stage;
        const ShaderCompileResult result = driver->compile(req);
        if (!result.ok) {
            std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
            _impl->lastCompileErrors.push_back(result.stderrText);
            return false;
        }
        out = result.bytes;
        return !out.empty();
    };

    CompiledShaderProgram prog;
    if (!compileStage("vertex", vertexSc, prog.vsBin)
        || !compileStage("fragment", fragmentSc, prog.fsBin)) {
        return ShaderResource{};
    }

    tryInjectBoneUniformArray(vertexSc, prog);
    tryInjectSkeletonUniformBlock(vertexSc, prog);
    tryInjectDeclaredUniforms(vertexSc, prog);
    tryInjectDeclaredUniforms(fragmentSc, prog);
    tryInjectTextureBindings(fragmentSc, prog);
    prog.success       = true;

    ShaderResource resource = acquire(prog);
    if (!resource.isValid()) {
        return resource;
    }

    // R-H-02 race: re-check cache after compile/wire-up under exclusive lock.
    std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
    const auto existing = _impl->cache.find(key);
    if (existing != _impl->cache.end()) {
        const std::shared_ptr<ShaderResourceImpl> cached =
            _impl->resolveLocalShared(detail::shaderHandleLocalId(existing->second));
        if (cached != nullptr && bgfx::isValid(cached->programHandle)) {
            _impl->handles.invalidate(detail::shaderHandleLocalId(resource.id()));
            resource.reset();
            ++_impl->stats.binaryHits;
            return ShaderResource(existing->second);
        }
    }
    _impl->cache[key] = resource.id();
    ++_impl->stats.binaryMisses;
    return resource;
}

ShaderResource ShaderResourcePool::acquire(const CompiledShaderProgram& prog)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

    if (!prog.success) {
        return ShaderResource{};
    }

    auto impl = std::make_unique<ShaderResourceImpl>();
    std::vector<std::string> errors;
    if (!detail::wireUpProgram(*impl, prog, errors)) {
        // R-H-01 audit fix (2026-08-26): `wireUpProgram` cleans up the
        // bgfx program handle on failure, but partial-binding entries
        // (uniform handles created during buildBindingTable) survive
        // destruction.  Always `destroyGpuResources()` on the failed
        // impl to release those slots too — the function is safe to
        // call on a half-populated impl.
        impl->destroyGpuResources();
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        _impl->lastCompileErrors = errors;
        return ShaderResource{};
    }

    {
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        _impl->lastCompileErrors.clear();
    }

    // R-H-05 acquire-once idempotency (2026-08-26): inserting the handle
    // into the table is the only step that touches shared state outside
    // `_impl->mutex`.  `makeHandle` calls `handles.insert`, which itself
    // takes `ShaderHandleTable::_mutex` (nested under our exclusive
    // `_impl->mutex`); ordering is consistent — pool-mutex always first.
    const uint64_t handle = _impl->makeHandle(std::move(impl));
    return ShaderResource(handle);
}

void ShaderResourcePool::shutdown()
{
    if (_impl) {
        std::unique_lock<std::shared_mutex> implLock(_impl->mutex);
        _impl->shutdownAll();
    }
}

std::atomic<uint32_t> ShaderResourcePool::Impl::s_nextPoolSerial{1};

} // namespace ayt::shader
