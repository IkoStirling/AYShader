# AYShader

> **Phoskia** — φῶς (光) + σκιά (影)，光与影的交织，shader 的本质。

AYShader 是 AY Engine 的着色器子系统：接受 **Phoskia** DSL 源码，经过完整的编译器流水线（**Lex → Parse → AST → 可选 SemA → Backend**），生成目标平台 shader 代码（当前仅 BGFX `.sc`，再交给 shaderc 编译为平台二进制）。

完整设计见 [`design.md`](design.md)。本文档是面向开发者的概览。

---

## 状态

**Phase 4 已关闭**（Phase 3.6 已关闭；Phase 5 小步切片已落地，测试全绿后更新计数）。

| Phase | 范围 | 状态 |
|---|---|---|
| Phase 1 | Lexer + Parser + AST + BGFX 后端 + shaderc e2e | ✅ |
| Phase 2.1 | Variant attribute `#ifdef` opt-in | ✅ |
| Phase 2.2 | TypeInference + SemanticAnalyzer 完整化 + 内置函数库 | ✅ |
| Phase 2.3 | PBR 函数库（Fresnel / GGX / Smith） | ✅ |
| Phase 2.4 | Parser panic-mode 错误恢复 | ✅ |
| Phase 2.5 | Token 降级重构（`vec3`/`float`/... → `Identifier`） | ✅ |
| Phase 2 收尾 | Golden file 验证 + PBR 端到端 demo | ✅ |
| Phase 2.6 | Compute declaration (`compute Foo { }`) — AST + Parser + BGFX stub | ✅ |
| Phase 3.1 | Phoskia IR (`AYIr`) + AST→IR 降级 + BGFX retarget | ✅ |
| Phase 3.2-pre | Compiler out-param 重构（SSO NRVO 根因修复） | ✅ |
| Phase 3.2 | Compute 端到端落地（BGFX `.sc` compute emit + storage buffer + thread-id） | ✅ |
| Phase 3.3 | `[numthreads]` attribute + `uint` builtin + `uvec3` strict + `groupshared` | ✅ |
| Phase 3.4 | UBO 表面语法（`uniformblock` + `layout(std140, binding = N)` + 全平台 `-p 430`） | ✅ |
| Phase 3.5-A | Storage binding 表面语法（`storage X : rwstructuredbuffer<T> binding N;` → `layout(std430, binding = N)`） | ✅ |
| Phase 3.5-B | UBO binding 表面语法（`uniformblock X { ... } binding N;` → `layout(std140, binding = N)`） | ✅ |
| Phase 3.6 | 产品化：`compileToProgram()` + `.sc` 内化 + `AYShadercDriver` | ✅ |
| Phase 4-A | `ShaderResource` opaque handle + 最小 `ShaderResourcePool` wire-up | ✅ |
| Phase 4-B | Pool 引擎配置 + `compile()` / `acquire(src)` + 内存 cache + `release()` | ✅ |
| Phase 4-C | `Compiler::compileToShaderResource(src[, opts], pool)` 一站式 compile | ✅ |
| Phase 4-E | `AYShader/ShaderProgram.h` 剥离 bgfx；legacy `ShaderProgram` → `detail/` | ✅ |
| Phase 4-G | `AYShader.h` 不再 include legacy cache/converter（frontend 零 bgfx） | ✅ |
| Phase 4-D | std140 layout 内化 + `getUniformBlockSize` / field offset API | ✅ |
| Phase 4-I | 磁盘 cache tier（`.aysc`）收编进 `ShaderResourcePool`；key = SHA256 | ✅ |
| Phase 4-F | `DrawCallContext::state` + `submit()` 帧期 `bgfx::setState` | ✅ |
| Phase 4-J | hot-reload：`compileFromFile` + `pollHotReload`（mtime + 100ms debounce） | ✅ |
| Phase 4-K | `Test_ShaderCacheIntegration` — frontend TU 不含 `<bgfx/bgfx.h>` | ✅（contract 层） |
| Phase 4-H | `.sc` stage text 移入 `detail/`；public API 不再暴露 `.sc` 字段 | ✅ |
| Phase 4-L | `ShaderCapability` + `pool.require()` | ✅ |
| Phase 4-M | `setAutoProbeFromRendererType` + renderer→platform/profile 探测 | ✅ |
| Phase 4-N | `PhoskiaDiagnostic` 结构化错误 + UBO 名称语义注册 | ✅ |
| Phase 4-O | `ShaderResource` 8-byte opaque handle + handle table | ✅ |
| Phase 4-P | neutral source keys（`vertex_stage_N` / `varying_definitions`） | ✅ |
| Phase 4-Q | 两级 cache（IR + binary）+ `CacheStats` | ✅ |
| Phase 4-R | `CompileOptions` 精简（`defines` / debug flags only） | ✅ |
| Phase 5 | Fragment 导数（`dFdx` / `dFdy` / `fwidth`） | ✅ |
| Phase 5 | `texturecube` + `SAMPLERCUBE` + `sample(tex, vec3)` → `textureCube` | ✅ |
| Phase 5 | Storage image / texture3d / barrier / atomic 等 | 🅿 延后 |
| Phase 5+ | HLSL 后端（按需 — DXC 一手质量 / 减体积） | 🅿 暂缓 |
| Phase 5+ | WGSL 后端（按需 — WebGPU 目标） | 🅿 暂缓 |

---

## 测试

```bash
# Build（CMake preset: D:\Projects\out\build\x64-Debug）
cmake --build D:\Projects\out\build\x64-Debug --target AYShader_Test

# Windows：若裸跑 cl 报 cstdint 找不到，先初始化 MSVC 环境再构建：
cmd /c "\"D:\Visual Studio\Product\VC\Auxiliary\Build\vcvars64.bat\" && cmake --build D:\Projects\out\build\x64-Debug --target AYShader_Test"

# 若改动了 Pool/wire-up/测试二进制相关代码后测试异常退出（0xC0000005），先 clean 再编：
cmake --build D:\Projects\out\build\x64-Debug --target clean
cmake --build D:\Projects\out\build\x64-Debug --target AYShader_Test

# 跑全部测试
D:\Projects\out\build\x64-Debug\AYRuntime\AYShader\unittest\AYShader_Test.exe
```

最后一次完整跑：**1080 / 1080 PASS**（Phase 5 小步切片）。

### 测试套件

| 文件 | 覆盖 |
|---|---|
| `Test_Lexer.cpp` | Token 类型识别、关键字、数字字面量、字符串、变体属性 |
| `Test_Parser.cpp` | material/property/uniform/texture 声明、shader block、expression |
| `Test_Phoskia.cpp` | 端到端 `Compiler::compile` 通过 |
| `Test_BGFXConverter.cpp` | BGFX 后端输出字符串、three-piece、varying.def.sc |
| `Test_ShaderCompile.cpp` | 端到端 **shaderc** 编译 vs/fs/cs 到 `.bin`（Phase 3.2 含 compute 端到端），含完整 PBR demo |
| `Test_TypeInference.cpp` | 字面量 / 二元 / swizzle / 索引 / builtin / constructor / unify |
| `Test_SemanticAnalyzer.cpp` | ShaderParam 注册、property 推断、严格 vec4 / bool 检查 |
| `Test_ParserRecovery.cpp` | panic-mode：synchronize() 跳过到 statement boundary |
| `Test_PBRFunctions.cpp` | FresnelSchlick / GGX / Smith 的数学性质 |
| `Test_BuiltinTypes.cpp` | `AYBuiltinTypes::isBuiltinType` 全覆盖 |
| `Test_GoldenFiles.cpp` | 5 个 Phoskia fixture 输出 byte-equal 比对 baseline |
| `Test_IrGenerator.cpp` | Phase 3.1 IR 层：AST→IR 降级 + resolvedType + 完整 BGFX retarget |
| `Test_ShaderResource.cpp` | Phase 4 `ShaderResource` / `ShaderResourcePool` / `compileToShaderResource` |
| `Test_Std140Layout.cpp` | Phase 4-D std140 UBO layout calculator |
| `Test_ShaderDiskCache.cpp` | Phase 4-I disk cache roundtrip + pool persistence |
| `Test_ShaderHotReload.cpp` | Phase 4-J hot-reload debounce + compileFromFile |
| `Test_ShaderCacheIntegration.cpp` | Phase 4-K frontend header contract（本 TU 不含 bgfx） |
| `Test_CapabilityDispatch.cpp` | Phase 4-L capability 位掩码 |
| `Test_DiagnosticStructure.cpp` | Phase 4-N `PhoskiaDiagnostic::toHumanString()` |
| `Test_HandleABI.cpp` | Phase 4-O opaque handle ABI |
| `Test_CacheStats.cpp` | Phase 4-Q 两级 cache 统计 |
| `Test_Phase5Slice.cpp` | Phase 5 小步：`dFdx`/`dFdy`/`fwidth`、`texturecube` emit |

### Golden fixture

`unittest/golden/{name}.phoskia` 是源，`{name}.sc` 是期望输出。首次跑测试时若 baseline 不存在则**自动生成**；之后用 `AY_SHADER_REGEN_GOLDEN=1` 强制重生成。

| Fixture | 覆盖能力 |
|---|---|
| `unlit` | 最简 material（property + return） |
| `pbr_minimal` | in/out + normalize/dot/max 链 + variant attribute |
| `pbr_with_emission` | `[variant useEmission]` 包裹代码段 |
| `pbr_with_texture` | texture2d + sample + swizzle |
| `pbr_full` | 完整 PBR 演示：2 个 texture、2 个 property、5 个 PBR 内置、clearcoat、IBL diffuse、emission variant |
| `empty` | 空 vertex/fragment body，fence-only 输出 |
| `compute_minimal` | Phase 3.2 compute：storage buffer + thread_id + `counters[idx] = counters[idx] + 1`（GPGPU kernel smoke test） |
| `compute_with_storage_binding` | Phase 3.5-A compute：2 个 storage decl 显式 `binding 0` / `binding 1`，emit `layout(std430, binding = N)` |

---

## Phoskia 示例

```phoskia
material PBR {
    texture2d albedoMap
    uniform mat4 modelViewProj
    uniform vec3 cameraPos

    vertex {
        in pos    : position
        in nrm    : normal
        in uv     : texcoord
        out worldNormal : normal = vec3(0.0, 0.0, 1.0)
        out uvCoord     : texcoord = vec2(0.0, 0.0)
        return vec4(pos, 1.0)
    }

    fragment {
        in worldNormal : normal
        in uvCoord     : texcoord
        let N = normalize(worldNormal)
        let V = normalize(cameraPos)
        let baseColor = sample(albedoMap, uvCoord)
        let NdotL = max(dot(N, V), 0.0)
        [variant useEmission]
        result = result + emission
        return vec4(baseColor.rgb * NdotL, 1.0)
    }
}
```

→ 编译为 BGFX 三段输出（`vs_PBR.sc` + `fs_PBR.sc` + `varying.def.sc`），再交给 `shaderc.exe` 编译为平台二进制。

**Compute declaration (Phase 3.2 ✅):**

```phoskia
material PBR { vertex { } fragment { } }   // existing material, unchanged

compute Increment {                        // 顶层 compute，GPGPU kernel
    storage counters : rwstructuredbuffer<int>
    let idx = thread_id.x                   // gl_GlobalInvocationID.x
    counters[idx] = counters[idx] + 1
}
```

`compute Name { <body> }` 是与 `material` 平级的顶层声明，定义 GPGPU kernel（粒子模拟、图像处理、GPU 剔除、lightmap 烘焙等）。

**BGFX `.sc` 是 compute 的目标后端**（`shaderc --type compute` 直接支持；`bgfx::createProgram(ShaderHandle _csh)` 重载 + `bgfx::dispatch(_handle, ...)` 走整 dispatch）。Phase 3.2 实现的 compute 端到端能力：

- **`convertComputeDecl`** — Phoskia compute → BGFX `.sc`（`layout(local_size_x = 64) in;` + `void main() { <body> }`）
- **`storage NAME : structuredbuffer<T>` / `rwstructuredbuffer<T>`** — GPGPU 存储缓冲声明；GLSL 路径 emit 为 `buffer Name { T data[]; } Name;`（read / read-write 在 GLSL 路径下形态相同，access 字段在 IR 保留供未来 HLSL emitter 区分 `StructuredBuffer<T>` vs `RWStructuredBuffer<T>`）
- **`storage NAME : rwstructuredbuffer<T> binding N;`** (Phase 3.5-A ✅) — 可选显式 binding 后缀；emit `layout(std430, binding = N) buffer ...;`。无 binding 时按声明顺序自动分配（从 0 起，避开已显式占用的 slot）。duplicate binding 编译期报错
- **`thread_id` / `group_id` / `dispatch_id`** — 0-arg 内置，返回 `vec3`；分别映射为 `gl_GlobalInvocationID` / `gl_WorkGroupID` / `(gl_NumWorkGroups * gl_WorkGroupID)`
- **`shaderc --type compute` e2e** — `Test_ShaderCompile.cpp::shaderc_compiles_compute_with_storage_buffer` 跑通整条 Phoskia → BGFX `.sc` → shaderc `.bin` 链路，断言 `.bin` 非空（`bgfx::createProgram(_csh)` 拒收 0 字节 program）

Phase 3.2 限制：`numthreads` 硬编 64（待 Phase 3.3 加 `[numthreads(X, Y, Z)]` attribute）、元素类型仅限 builtin scalar/vector（`uint` / 自定义 `struct` 待 Phase 3.3）、`thread_id` 系列以 `vec3` 表示（strict `uvec3` 待 Phase 3.3）。

**Uniform block object (UBO, Phase 3.4 ✅):**

```phoskia
// 顶层（与 material / compute 同级）
uniformblock Camera {
    vec3 position
    vec3 direction
    float fov
}
uniformblock Lighting {
    vec3 ambient
    float sunIntensity
}

material PBR {
    vertex {
        return vec4(Camera.position, 1.0)
    }
    fragment {
        return vec4(Lighting.ambient * Lighting.sunIntensity, 1.0)
    }
}
```

→ GLSL emit：
```glsl
layout(std140, binding = 0) uniform Camera { vec3 position; vec3 direction; float fov; } Camera;
layout(std140, binding = 1) uniform Lighting { vec3 ambient; float sunIntensity; } Lighting;
```

**为什么**：N 个分散 `uniform float x;` = N 次 `bgfx::setUniform()` API call。UBO 把所有字段打包成一个 buffer，引擎一次 `bgfx::setUniform(handle, ptr, sizeof(block))` 上传。生产 PBR 一般 30-50 个 uniform，UBO 后变成 1-3 次 driver call / draw。

**约束**：
- 字段限于 builtin 标量/向量（`float` / `int` / `uint` / `vec2-4` / `ivec2-4` / `uvec2-4` / `mat3-4`）
- 块名 = instance 名（`Camera.position` 是 `MemberExpr` 访问）
- binding slot 编译器自动分配（按声明顺序 0, 1, 2, ...）
- shaderc profile bump 到 `-p 430`（`binding = N` 语法要求 GLSL 4.30+）—— 同时也把 material / compute 整个 e2e 套件统一到 4.30
- 已知 limitation：UBO 字段的 strict type-check 暂不在 Phoskia 端做（`let p = Camera.position` 推断为 TypeVar 而非 `vec3`）；emit 路径透明，GLSL 编译器负责类型检查。完整 struct 推断留 Phase 4+ 跟 struct 类型系统一起做

完整设计见 [`design.md`](design.md) §6.6 + §6.6.1 (emit shape) + §6.6.2 (storage) + §6.6.3 (thread-id) + §6.7 (UBO, 计划 §11.4 收录)。

---

## 项目结构

```
AYShader/
├── README.md           # 本文件
├── design.md           # 完整设计文档（含类型推导规则、语法 BNF、错误码表）
├── CMakeLists.txt
├── include/
│   ├── AYShader\Token.h
│   ├── AYShader\Lexer.h
│   ├── AYShader\Parser.h
│   ├── AYShader\Ast.h
│   ├── AYShader/Type.h
│   ├── AYShader/TypeInference.h
│   ├── AYShader\SemanticAnalyzer.h
│   ├── AYShader/BuiltinTypes.h
│   ├── AYShader/BuiltinFunctions.h
│   ├── AYShader\CompilerError.h
│   ├── AYShader/Phoskia.h
│   ├── AYShader/Ir.h
│   ├── AYShader/IBackendConverter.h
│   ├── AYShader/BGFXConverter.h
│   ├── AYShader/ShaderProgram.h
│   ├── AYShader/ShaderResource.h
│   ├── AYShader/ShaderResourcePool.h
│   └── AYShader/ShaderCache.h
├── src/                # 一一对应实现
├── unittest/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── Test_*.cpp
│   └── golden/         # Phoskia fixture + expected .sc baseline
└── thirdParty/
    └── bgfx-install/   # vendored bgfx（用于 shaderc）
```

---

## 依赖

- **bgfx**（vendored，第三方）—— 仅用于 `shaderc.exe` 路径，AYShader 本身不链接 bgfx
- **AYTest**（同 repo `AYFoundation/AYTest`）—— 极简单元测试框架

---

## License

Internal — AY Engine project.