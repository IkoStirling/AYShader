// AYShaderResourcePool.cpp - wire-up, compile path, cache, pool lifetime

#include "AYShaderResourcePool.h"
#include "AYBGFXConverter.h"
#include "AYPhoskia.h"
#include "ShaderResourceImpl.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::shader
{

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

        const uint16_t numVec4 = static_cast<uint16_t>(
            block.fieldNames.empty() ? 1u : block.fieldNames.size());
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
    std::vector<std::string> bgfxIncludeDirs;
    std::string cacheDirectory;
    bool hotReloadEnabled = false;

    std::vector<std::shared_ptr<ShaderResourceImpl>> resources;
    std::unordered_map<std::string, std::weak_ptr<ShaderResourceImpl>> cache;

    BGFXCompileOptions engineBgfxOpts() const
    {
        BGFXCompileOptions opts;
        opts.shadercPath = shadercPath;
        opts.platform = platform;
        opts.profile = profile;
        opts.includeDirs = bgfxIncludeDirs;
        return opts;
    }

    std::string makeCacheKey(const std::string& keyOverride,
                             const std::string& src,
                             const phoskia::CompileOptions& opts) const
    {
        if (!keyOverride.empty()) {
            return keyOverride;
        }
        std::ostringstream oss;
        oss << platform << '|' << profile << '|' << shadercPath << '|';
        for (const std::string& dir : bgfxIncludeDirs) {
            oss << dir << ';';
        }
        oss << '|' << opts.enableTypeInference
            << opts.enableSemanticAnalysis
            << opts.keepSources
            << opts.dumpIntermediate
            << '|' << src;
        return oss.str();
    }

    void evictStaleCacheEntries()
    {
        for (auto it = cache.begin(); it != cache.end(); ) {
            if (it->second.expired()) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void track(std::shared_ptr<ShaderResourceImpl> impl)
    {
        resources.push_back(std::move(impl));
    }

    void untrack(const ShaderResourceImpl* ptr)
    {
        resources.erase(
            std::remove_if(resources.begin(), resources.end(),
                           [ptr](const std::shared_ptr<ShaderResourceImpl>& p) {
                               return p.get() == ptr;
                           }),
            resources.end());
    }

    void removeFromCache(const ShaderResourceImpl* ptr)
    {
        for (auto it = cache.begin(); it != cache.end(); ) {
            const std::shared_ptr<ShaderResourceImpl> locked = it->second.lock();
            if (!locked || locked.get() == ptr) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void shutdownAll()
    {
        for (const std::shared_ptr<ShaderResourceImpl>& impl : resources) {
            if (impl) {
                impl->destroyGpuResources();
            }
        }
        resources.clear();
        cache.clear();
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
}

void ShaderResourcePool::setGLSLProfile(const std::string& profile)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }
    _impl->profile = profile;
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

ShaderResource ShaderResourcePool::compile(const std::string& src)
{
    return acquire(src, phoskia::CompileOptions{}, "");
}

ShaderResource ShaderResourcePool::compile(const std::string& src,
                                           const phoskia::CompileOptions& opts)
{
    return acquire(src, opts, "");
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

    _impl->evictStaleCacheEntries();
    const std::string key = _impl->makeCacheKey(cacheKey, src, opts);

    if (const auto it = _impl->cache.find(key); it != _impl->cache.end()) {
        if (std::shared_ptr<ShaderResourceImpl> cached = it->second.lock()) {
            if (bgfx::isValid(cached->programHandle)) {
                return ShaderResource(cached);
            }
        }
    }

    phoskia::Compiler compiler;
    CompiledShaderProgram prog;
    compiler.compileToProgram(src, opts, _impl->engineBgfxOpts(), prog);

    ShaderResource res = acquire(prog);
    if (res.isValid()) {
        _impl->cache[key] = res._impl;
    }
    return res;
}

void ShaderResourcePool::release(ShaderResource& res)
{
    if (!res.isValid() || !_impl) {
        res.reset();
        return;
    }

    std::shared_ptr<ShaderResourceImpl> impl = res._impl;
    impl->destroyGpuResources();
    _impl->untrack(impl.get());
    _impl->removeFromCache(impl.get());
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

    auto impl = std::make_shared<ShaderResourceImpl>();
    std::vector<std::string> errors;
    if (!detail::wireUpProgram(*impl, prog, errors)) {
        impl->destroyGpuResources();
        return ShaderResource{};
    }

    _impl->track(impl);
    return ShaderResource(std::move(impl));
}

void ShaderResourcePool::shutdown()
{
    if (_impl) {
        _impl->shutdownAll();
    }
}

} // namespace ayt::shader
