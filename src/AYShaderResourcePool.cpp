// AYShaderResourcePool.cpp - wire-up, compile path, cache, pool lifetime

#include "AYShaderResourcePool.h"
#include "AYBGFXConverter.h"
#include "AYPhoskia.h"
#include "ShaderResourceImpl.h"
#include "detail/AYShaderDigest.h"
#include "detail/AYShaderDiskCache.h"
#include "detail/AYShaderFileWatch.h"
#include "detail/AYShaderHandleEncoding.h"
#include "detail/AYShaderHandleTable.h"
#include "AYIr.h"

#include <AYFile.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <bgfx/bgfx.h>

namespace ayt::shader
{

namespace {

constexpr int64_t kHotReloadDebounceMs = 100;

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
        profile = "s_6_0";
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
    case bgfx::RendererType::WebGPU:
        platform = "linux";
        profile = "wgsl";
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

bgfx::ShaderHandle createShaderFromBytes(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
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
        entry.uniformHandle = bgfx::createUniform(
            block.name.c_str(), bgfx::UniformType::Vec4, numVec4);

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
        entry.uniformHandle = bgfx::createUniform(
            uniform.name.c_str(),
            *bgfxType,
            uniformElementCount(uniform.type, uniform.count));

        const BindingId id = allocateBinding(nextId, impl, entry);
        impl.uniformBindings.emplace(uniform.name, id);
    }

    for (const BGFXTexture& texture : prog.textures) {
        BindingEntry entry;
        entry.kind = BindingKind::Texture;
        entry.name = texture.name;
        entry.textureBinding = texture.binding;
        entry.uniformHandle = bgfx::createUniform(
            texture.name.c_str(), bgfx::UniformType::Sampler);

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
        impl.vertexShader = createShaderFromBytes(prog.vsBin);
        impl.fragmentShader = createShaderFromBytes(prog.fsBin);
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
        impl.computeShader = createShaderFromBytes(prog.csBin);
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

    return buildBindingTable(impl, prog, errors);
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
    static std::atomic<uint32_t> s_nextPoolSerial;
    static std::unordered_map<uint32_t, Impl*> s_poolRegistry;
    static std::mutex s_poolRegistryMutex;

    static void registerPool(uint32_t serial, Impl* impl)
    {
        std::lock_guard<std::mutex> lock(s_poolRegistryMutex);
        s_poolRegistry[serial] = impl;
    }

    static void unregisterPool(uint32_t serial)
    {
        std::lock_guard<std::mutex> lock(s_poolRegistryMutex);
        s_poolRegistry.erase(serial);
    }

    static Impl* findPool(uint32_t serial)
    {
        std::lock_guard<std::mutex> lock(s_poolRegistryMutex);
        const auto it = s_poolRegistry.find(serial);
        return it != s_poolRegistry.end() ? it->second : nullptr;
    }

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
    std::string profile = "430";
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
        if (caps == nullptr) {
            return;
        }
        std::string probedPlatform = platform;
        std::string probedProfile = profile;
        mapRendererTypeToPlatformProfile(caps->rendererType, probedPlatform, probedProfile);
        if (!platformExplicit) {
            platform = probedPlatform;
        }
        if (!profileExplicit) {
            profile = probedProfile;
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

    ShaderResourceImpl* resolveLocal(uint32_t localId) const
    {
        return handles.resolve(localId);
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
        for (auto& [normalizedPath, watch] : hotReloadWatches) {
            (void)normalizedPath;
            const std::optional<int64_t> mtime = detail::fileMtimeMs(watch.sourcePath);
            if (!mtime.has_value()) {
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
            }
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
            ShaderResourceImpl* impl =
                resolveLocal(detail::shaderHandleLocalId(it->second));
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

ShaderResourcePool::ShaderResourcePool() = default;

ShaderResourcePool::~ShaderResourcePool()
{
    shutdown();
}

ShaderResourcePool::ShaderResourcePool(ShaderResourcePool&&) noexcept = default;

ShaderResourcePool& ShaderResourcePool::operator=(ShaderResourcePool&&) noexcept = default;

void ShaderResourcePool::setShadercExecutable(const std::string& path)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->shadercPath = path;
}

void ShaderResourcePool::setBgfxIncludeDirs(const std::vector<std::string>& dirs)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->bgfxIncludeDirs = dirs;
}

void ShaderResourcePool::setPlatform(const std::string& platform)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->platform = platform;
    _impl->platformExplicit = true;
}

void ShaderResourcePool::setGLSLProfile(const std::string& profile)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->profile = profile;
    _impl->profileExplicit = true;
}

void ShaderResourcePool::setCacheDirectory(const std::string& path)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->cacheDirectory = path;
}

void ShaderResourcePool::setHotReloadEnabled(bool enabled)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->hotReloadEnabled = enabled;
}

void ShaderResourcePool::require(ShaderCapability capability)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->requiredCaps = capability;
}

void ShaderResourcePool::setAutoProbeFromRendererType(bool enabled)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->autoProbe = enabled;
}

void ShaderResourcePool::bindRendererTypeForTests(uint8_t bgfxRendererType,
                                                  const std::string& platform,
                                                  const std::string& profile)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
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
    return _impl->stats;
}

const std::vector<std::string>& ShaderResourcePool::lastCompileErrors() const
{
    static const std::vector<std::string> kEmpty;
    if (!_impl) {
        return kEmpty;
    }
    return _impl->lastCompileErrors;
}

ShaderResourceImpl* ShaderResourcePool::resolveHandle(uint64_t handle)
{
    if (handle == 0) {
        return nullptr;
    }
    Impl* pool = Impl::findPool(detail::shaderHandlePoolSerial(handle));
    if (pool == nullptr) {
        return nullptr;
    }
    return pool->resolveLocal(detail::shaderHandleLocalId(handle));
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

    std::string src;
    if (!detail::readTextFile(path, src)) {
        return ShaderResource{};
    }

    ShaderResource res = acquire(src, opts, "");
    if (res.isValid() && _impl->hotReloadEnabled) {
        const std::string key = _impl->makeCacheKey("", src, opts);
        _impl->registerHotReloadWatch(path, key, res.id());
    }
    return res;
}

void ShaderResourcePool::pollHotReload()
{
    if (_impl) {
        _impl->pollHotReloadWatches();
    }
}

ShaderResource ShaderResourcePool::acquire(const std::string& src,
                                           const std::string& cacheKey)
{
    return acquire(src, phoskia::CompileOptions{}, cacheKey);
}

ShaderResource ShaderResourcePool::acquire(const std::string& src,
                                           const phoskia::CompileOptions& opts,
                                           const std::string& cacheKey)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

    _impl->ensurePlatformProfileResolved();
    _impl->evictStaleCacheEntries();
    const std::string key = _impl->makeCacheKey(cacheKey, src, opts);
    const std::string sourceKey = _impl->makeSourceCacheKey(src);

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
    ++_impl->stats.binaryMisses;

    CompiledShaderProgram prog;
    if (!_impl->cacheDirectory.empty()) {
        const std::string diskPath =
            detail::diskCacheFilePath(_impl->cacheDirectory, key);
        if (detail::loadCompiledProgramFromDisk(diskPath, prog) && prog.success) {
            ShaderResource res = acquire(prog);
            if (res.isValid()) {
                _impl->cache[key] = res.id();
            }
            return res;
        }
    }

    phoskia::Compiler compiler;
    std::shared_ptr<const phoskia::ir::IRProgram> cachedIr;
    if (const auto irIt = _impl->sourceCache.find(sourceKey); irIt != _impl->sourceCache.end()) {
        cachedIr = irIt->second;
        ++_impl->stats.sourceHits;
    } else {
        ++_impl->stats.sourceMisses;
        phoskia::ir::IRProgram generated;
        std::vector<std::string> irErrors;
        if (!compiler.generateIr(src, opts, generated, irErrors)) {
            prog.success = false;
            prog.errors = std::move(irErrors);
            _impl->lastCompileErrors = prog.errors;
            return ShaderResource{};
        }
        cachedIr = std::make_shared<const phoskia::ir::IRProgram>(std::move(generated));
        _impl->sourceCache[sourceKey] = cachedIr;
    }

    shader::AYBGFXConverter converter;
    converter.compileToBinary(*cachedIr, _impl->engineBgfxOpts(opts), prog);

    if (prog.success && !_impl->cacheDirectory.empty()) {
        const std::string diskPath =
            detail::diskCacheFilePath(_impl->cacheDirectory, key);
        detail::saveCompiledProgramToDisk(diskPath, prog);
    }

    ShaderResource res = acquire(prog);
    if (res.isValid()) {
        _impl->cache[key] = res.id();
        _impl->lastCompileErrors.clear();
    } else if (!prog.errors.empty()) {
        _impl->lastCompileErrors = prog.errors;
    }
    return res;
}

void ShaderResourcePool::release(ShaderResource& res)
{
    if (res.id() == 0 || !_impl) {
        res.reset();
        return;
    }

    const uint32_t localId = detail::shaderHandleLocalId(res.id());
    _impl->handles.invalidate(localId);
    _impl->removeHandleFromCache(res.id());
    res.reset();
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
        impl->destroyGpuResources();
        _impl->lastCompileErrors = errors;
        return ShaderResource{};
    }

    _impl->lastCompileErrors.clear();

    const uint64_t handle = _impl->makeHandle(std::move(impl));
    return ShaderResource(handle);
}

void ShaderResourcePool::shutdown()
{
    if (_impl) {
        _impl->shutdownAll();
    }
}

std::atomic<uint32_t> ShaderResourcePool::Impl::s_nextPoolSerial{1};
std::unordered_map<uint32_t, ShaderResourcePool::Impl*>
    ShaderResourcePool::Impl::s_poolRegistry{};
std::mutex ShaderResourcePool::Impl::s_poolRegistryMutex{};

} // namespace ayt::shader
