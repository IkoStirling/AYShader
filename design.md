# AYShader Design

> **命名来源**：Phoskia — φῶς (光) + σκιά (影)，光与影的交织，shader 的本质。

## 1. 概述

AYShader 是 AY Engine 的**着色器子系统**。它接受 Phoskia 源码（一种高层次的 shader DSL），通过完整的编译器流水线（词法 → 语法 → AST → 可选语义分析 → 后端转换），生成目标平台的 shader 源码（目前仅 BGFX `.sc` 格式，再交由 shaderc 编译为平台二进制）。

### 1.1 设计目标

- **完整编译器**：词法分析 → 语法分析 → AST →（语义分析）→ 后端转换，不是简单字符串替换
- **人类友好的语法**：Phoskia 采用类型推导、命名参数、语义化关键字
- **多后端预留**：后端通过 `IAYBackendConverter` 接口注册，核心不耦合任何具体平台
- **可独立演进**：Phoskia 编译器与引擎集成层（ShaderProgram/ShaderCache/平台探测）解耦

### 1.2 在引擎中的位置

```
┌─────────────────────────────────────────────────────────────┐
│                       Game Engine                            │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌────────────────────────────────┐   │
│  │  AYResource     │───▶│           AYShader             │   │
│  └─────────────────┘    │                                │   │
│                         │  ┌────────────────────────┐    │   │
│                         │  │  Phoskia 编译器核心    │    │   │
│                         │  │  (ayt::shader::phoskia)│    │   │
│                         │  │  Token/Lexer/Parser/   │    │   │
│                         │  │  AST/SemanticAnalyzer  │    │   │
│                         │  └─────────┬──────────────┘    │   │
│                         │            │ 生成              │   │
│                         │            ▼                  │   │
│                         │  ┌────────────────────────┐    │   │
│                         │  │  后端转换器            │    │   │
│                         │  │  (ayt::shader)         │    │   │
│                         │  │  AYBGFXConverter       │    │   │
│                         │  │  + 未来 HLSL/GLSL/WGSL │    │   │
│                         │  └────────────────────────┘    │   │
│                         │  ┌────────────────────────┐    │   │
│                         │  │  引擎集成              │    │   │
│                         │  │  (ayt::shader)         │    │   │
│                         │  │  ShaderProgram/Cache   │    │   │
│                         │  └────────────────────────┘    │   │
│                         └────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 1.3 命名空间分层

| 命名空间 | 职责 | 文件 |
|---|---|---|
| `ayt::shader` | 引擎集成层：BGFX 后端、ShaderProgram、ShaderCache、Platform 枚举 | `IAYBackendConverter.h`、`AYBGFXConverter.h`、`AYShaderProgram.h`、`AYShaderCache.h` |
| `ayt::shader::phoskia` | Phoskia 编译器核心：Token/Lexer/Parser/AST/Semantic/Compiler/BuiltinFunctions | `AYToken.h`、`AYLexer.h`、`AYParser.h`、`AYAst.h`、`AYType.h`、`AYTypeInference.h`、`AYSemanticAnalyzer.h`、`AYCompilerError.h`、`AYPhoskia.h`、`AYBuiltinFunctions.h` |

**为什么这样分层**：

- Phoskia 是**一种语言**，`ayt::shader::phoskia` 是它的命名空间（参考 `ayt::shader::phoskia::Compiler`）。
- 后端转换器、ShaderProgram、ShaderCache 属于**引擎集成**，它们依赖具体的渲染后端（BGFX），不应该和 Phoskia 编译器混在一起。
- AST 节点**只**在 `ayt::shader::phoskia` 下，`ayt::shader` 的代码用 `phoskia::Program`、`phoskia::MaterialDecl` 等全限定名引用。
- 未来若新增其他 Phoskia 后端（如 `phoskia::lua`），只需新增一个 `IAYBackendConverter` 实现，不需要改 Phoskia 核心。

### 1.4 适用范围

**Phoskia 是 shader 专用 DSL，不是通用语言**。本文档不再沿用早期版本中"general-purpose dataflow language" 的描述。Phoskia 的设计目标明确：

- 输入：`.phoskia` 源文件
- 输出：目标平台 shader 源码（`.sc` for BGFX，未来 `.hlsl`/`.glsl`/`.wgsl`）
- 用户：引擎开发者、技术美术
- 错误容忍：严格（编译失败 = 资源不可用）

## 2. 编译流程

```
┌────────────────────────────────────────────────────────────┐
│                      完整编译流程                            │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  1. Phoskia 源码 (.phoskia 文件)                            │
│                                                            │
│  2. 词法分析 (Lexer) → Token 流                            │
│                                                            │
│  3. 语法分析 (Parser) → Program AST                        │
│                                                            │
│  4. 语义分析 (SemanticAnalyzer) — 可选，Phase 2+            │
│                                                            │
│  5. 后端转换 (IAYBackendConverter) → 目标 shader 源码      │
│                                                            │
│  6. shaderc → 平台二进制                                   │
│                                                            │
│  7. bgfx_create_shader() → GPU 可执行格式                 │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

## 3. 核心架构

### 3.1 Phoskia 编译器核心

```
┌────────────────────────────────────────────────────────────┐
│  ayt::shader::phoskia                                      │
│                                                            │
│  AYToken.h         — Token / TokenType                     │
│  AYLexer.h/.cpp    — 词法分析（关键字表、字符流、行号）    │
│  AYParser.h/.cpp   — 递归下降语法分析（错误恢复）          │
│  AYAst.h           — 表达式 + 语句 + 声明节点              │
│  AYType.h/.cpp     — 类型系统（primitive/vector/matrix/    │
│                       array/function/struct）              │
│  AYTypeInference.h — Hindley-Milner 风格类型推导           │
│  AYSemanticAnalyzer.h/.cpp — 作用域/类型检查/标识符解析   │
│  AYCompilerError.h — 错误聚合（Code/Message/Line/Column） │
│  AYBuiltinFunctions.h/.cpp — 内置数学/PBR/纹理函数注册   │
│  AYPhoskia.h/.cpp  — Compiler 流水线编排                   │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### 3.2 后端与引擎集成

```
┌────────────────────────────────────────────────────────────┐
│  ayt::shader                                               │
│                                                            │
│  IAYBackendConverter.h    — 后端接口（抽象）               │
│  AYBGFXConverter.h/.cpp   — BGFX 后端（Phoskia → .sc）     │
│  AYShaderProgram.h/.cpp   — 编译后的程序                   │
│  AYShaderCache.h/.cpp     — 编译缓存                       │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### 3.3 后端注册机制

`Compiler` 不硬编码任何后端。后端通过工厂函数注册：

```cpp
Compiler compiler;
compiler.registerBackend("bgfx", []() {
    return std::make_unique<AYBGFXConverter>();
});

auto result = compiler.compileToBackend(source, "bgfx");
```

`bgfx` 是唯一默认注册的后端。Phase 1 的 `Compiler` 构造函数自动注册 `bgfx`，调用方可注册更多。

### 3.4 错误传播

每个阶段（Lexer / Parser / SemanticAnalyzer / BackendConverter）独立维护自己的错误，通过 `Compiler` 统一聚合到 `CompileResult.errors`。调用方用单一入口检查错误，不需要分别访问每个子系统。

## 4. IR（中间表示） — TODO Phase 3

> 状态：**未实现**。设计已记录，作为未来 Phase 3 的工作。

原计划 Phoskia 拥有自己的 IR（Load/Store/Add/Mul/Phi/SSA 形式），位于 AST 和后端之间，用于跨后端优化和 SSA 转换。`AYPhoskia.h` 中以注释形式保留了原始设计草案。

**当前策略**：Phase 1/2 直接从 AST 生成后端代码（仅 BGFX），不做 IR、不做 SSA、不做优化。这个决定的原因：

1. 只有一个后端时，IR 是中间开销
2. AST 本身已经携带足够的语义信息（vec3、texture、shading block）
3. IR 的实现复杂度（SSA 构造器、支配树、Phi 节点）远超当前需求
4. 未来若需要优化或新增第二个后端，再实现 IR 仍有价值

## 5. 目录结构

```
AYShader/
├── design.md
├── CMakeLists.txt
├── AYShader.h                  # 主入口
└── include/
    └── AY*.h / IAY*.h          # 公开头文件（无二级目录）
```

源文件位于 `src/`，命名与头文件一一对应：

```
src/
├── AYLexer.cpp
├── AYParser.cpp
├── AYCompilerError.cpp
├── AYType.cpp
├── AYTypeInference.cpp
├── AYSemanticAnalyzer.cpp
├── AYBuiltinFunctions.cpp
├── AYPhoskia.cpp
├── AYBGFXConverter.cpp
├── AYShaderProgram.cpp
└── AYShaderCache.cpp
```

## 6. Phoskia 语法（精选示例）

最小材质：

```phoskia
material Unlit {
    property color = vec4(1.0, 0.0, 0.0, 1.0)
    vertex {
        return vec4(0.0, 0.0, 0.0, 1.0)
    }
    fragment {
        return color
    }
}
```

PBR：

```phoskia
material PBR {
    texture2d albedoMap
    uniform vec3 cameraPos

    vertex {
        in  pos    : position
        in  nrm    : normal
        in  uv     : texcoord
        in  clr    : color
        out worldNormal : normal = vec3(0.0, 0.0, 1.0)
        out baseColor   : color  = vec4(1.0, 0.0, 0.0, 1.0)
        out uvCoord     : texcoord = vec2(0.0, 0.0)

        let wpos = mul(u_modelViewProj, vec4(pos, 1.0))
        return wpos
    }

    fragment {
        in  worldNormal : normal
        in  baseColor   : color
        in  uvCoord     : texcoord

        let N = normalize(worldNormal)
        let L = normalize(cameraPos)
        let NdotL = max(dot(N, L), 0.0)
        return vec4(baseColor.rgb * NdotL, 1.0)
    }
}
```

### 6.1 块（vertex / fragment）

Phoskia 把每个 material 拆成**两个独立的 shader block**：`vertex { }` 和 `fragment { }`。
每个 block 内部以 `in / out` 声明开头（可选），随后是 statement 列表（`let`、`return`、`if`、`for` 等）。

**vertex / fragment 块的 return 语义。** `vertex { }` 里的 `return <expr>` 应当返回一个 vec4，后端 converter 会隐式把它绑定到 `gl_Position`（写入 `vs_*.sc`）。`fragment { }` 里的 `return <expr>` 应当返回一个 vec4，后端 converter 隐式绑定到 `gl_FragColor`（写入 `fs_*.sc`）。Phoskia 源码**绝不直接引用** `gl_Position` 或 `gl_FragColor`——这两个名字是 bgfx / GLSL 层的实现细节，对应"vertex 输出槽"和"fragment 输出槽"，由 converter 注入。

### 6.2 in / out 参数语义（Phoskia 抽象）

`in / out` 后跟的"语义类型"是 Phoskia 自己定义的 4 个关键字，**完全脱离 bgfx 概念**：

| Phoskia 关键字 | 含义 | GLSL 类型 | 默认值 |
|---|---|---|---|
| `position` | 顶点位置 / 世界空间位置 | `vec3` | `vec3(0.0, 0.0, 0.0)` |
| `normal`   | 法线                  | `vec3` | `vec3(0.0, 0.0, 1.0)` |
| `color`    | 顶点颜色 / 输出色     | `vec4` | `vec4(1.0, 0.0, 0.0, 1.0)` |
| `texcoord` | 纹理坐标              | `vec2` | `vec2(0.0, 0.0)` |

**Phoskia 程序员写的标识符（`pos` / `nrm` / `worldNormal` 等）是自由命名**，与 bgfx semantic 不直接挂钩。
后端 converter 负责把 Phoskia 语义映射到具体的 bgfx `a_*` / `v_*` 名字与硬件 semantic 槽位（POSITION / NORMAL / COLOR0 / TEXCOORD0）。

**Phase 1 限制**：每个语义类别在一个 block 内最多出现 1 次。Phase 2 引入 `texcoord0..7` / `color0..1` / `tangent` 等扩展槽位。

Variant 宏：

```phoskia
material PBR {
    [variant useEmission]
    property emission = vec3(0.0)

    fragment {
        in baseColor : color
        let result = baseColor.rgb
        [variant useEmission]
        result = result + emission
        return vec4(result, 1.0)
    }
}
```

### 6.3 Variant 宏语义

`[variant name]` 标记**可选代码段**。BGFX 后端把它展开为 C 预处理条件：

```glsl
#ifndef BGFX_VARIANT_<NAME_UPPERCASED>
// 跳过该段（默认行为）
#else
<实际代码>
#endif
```

`<NAME_UPPERCASED>` 把 `name` 中所有字母转大写、非字母数字字符替换为下划线，并在 **小写→大写边界**（camelCase 单词分界处）插入下划线。数字边界**不**插下划线——`HDR2Pass → BGFX_VARIANT_HDR2PASS`（数字紧贴字母不打断）。具体映射：`useEmission → BGFX_VARIANT_USE_EMISSION`、`HDR2Pass → BGFX_VARIANT_HDR2PASS`、`alpha-test → BGFX_VARIANT_ALPHA_TEST`、`skinning → BGFX_VARIANT_SKINNING`。

**默认关闭（opt-in）**——与 bgfx 自身的 variant 系统约定一致：variant 段在编译时被 `#ifndef` 跳过，**只有调用方显式传 `--define BGFX_VARIANT_<NAME>` 给 shaderc**，变体代码才会出现在最终 shader 里。

`[variant]` 在 vertex / fragment 块的 body 内任何位置都有效，可以与 `let` / `return` / `if` / `for` 等语句交错排列；多个连续 `[variant]` 形成嵌套（**第一个**先 `#ifndef`，**第二个**在内层再 `#ifndef`）。变体本身**不出现在输出文本**——它只切换后续代码的条件编译边界。

**与数组下标语法的消歧**：`parseCall` 的 LeftBracket 分支在看到 `[` 时会 lookahead 下一 token——如果是 `variant` 关键字就 break 出 `parseCall`、把 `[` 留给 `parseStatement` 处理。这是必要的，因为 `let x = a.b [variant foo]` 这种链式写法里 `[variant]` 是 statement 级别的 attribute，不是 expression 级别的数组下标。

完整 BNF 见 §10。

## 6.5 设计原则（反 AYJsonTokenHandler 灾难清单）

AYSerializer 中 `AYJsonTokenHandler` 的实现是一份反面教材——它用三个互斥的 `bool`（`_objectJustClosedInObject / _arrayJustClosedInObject / _fieldJustClosedInObject`）维护"延后决定要不要加逗号"的状态机；`writeObjectBegin / writeArrayBegin / writeField` 三处复制粘贴同一段"加逗号 + 缩进"逻辑；`readNextToken` 一函数 100+ 行里既递归 skipWhitespace 又重新分发；`TokenType::Field` 同时承担"已知 bool/null/数字"和"未知字符串"两类，写时还要按字符串形态再判定加不加引号；`readString` 一边转义一边返回字符串。**结果是单元测试覆盖几种组合后，运行时仍会在某些嵌套结构崩溃，且没有人能在 review 时一眼看出哪些组合已经覆盖**。

Phoskia 的 Lexer / Parser / 后端转换器在实现时**必须**遵守以下硬性约束，违反任何一条都视作必须重构：

1. **Token 是词法工件，只携带 `type / lexeme / 位置`**。禁止任何语义层字段出现在 Token 上——`literal`（已解析的 float/int/string/bool）、类型、属性、名字绑定都**不属于**词法阶段。`AYToken.h` 中 `Token` 故意没有 `literal` 字段，Lexer 不调用 `std::stof`、不做转义、不判定 bool/null 形态。

2. **每条 token 在 Lexer 内立即定型**，不允许"延后到下一条 token 才决定要不要加分隔符/换行"的状态机。如果未来 BGFX writer 或其他后端出现类似需求，必须用一个**显式的、有名字的、单元测试可枚举的状态结构**（例如 `IndentWriter`），而不是 2-3 个互斥 `bool`。`AYJsonTokenHandler` 的三 bool 状态机已经证明这条路的代价远超收益。

3. **Lexer / Reader 一处只做一件事**。字符读取、token 分类、值解释分别放在不同模块。不要在一个函数里既递归 `skipWhitespace` 又重新分发——把分发提出来成 `nextNonWhitespaceKind()` 之类的显式函数。

4. **类型分类放在产出类型的地方**。例如"是数字还是字符串"的判定在 Lexer 识别时就通过不同的 `TokenType`（`FloatLiteral` vs `StringLiteral`）定型，不要等到 writer 阶段再用 `find_first_not_of("0123456789.-+eE")` 反向猜。

5. **字符串字面量在 Lexer 阶段不做转义**。`readString` / `stringLiteral` 返回**原始切片**（引号已被剥离、但 `\"` 仍是 `\"`），转义由 Parser 或 Semantic 阶段处理。

6. **Token value 用 `std::string_view` 做中间传递**。Lexer 内部读到原始切片时用 `string_view`，最终落到 `Token::lexeme` 时再 `string` 构造一次，避免 `c_str() → string → Token → 又拷出` 的多次拷贝。`Token::lexeme` 本身保留 `std::string` 因为 token 要跨阶段生存。

7. **每个分支路径必须有单测**。AYJsonTokenHandler 之所以崩溃，是因为"空格分支""空栈分支""嵌套数组分支"这些次要路径没有单测保护。Phoskia 的 Lexer/Parser/后端转换器每新增一条分支路径，必须先补 unnitest，再写实现——这个顺序反了也不行（先写实现再补测试会忘掉边界）。

8. **新增任何"延后决定"机制前，先论证是否真的需要**。很多时候把决策点提前到 token 输出时反而更简单。AYJsonTokenHandler 的延后机制本质是想统一处理 pretty-print 的换行/缩进，但代价就是三处同步的 bool 状态机——不值。

9. **重复的字面量用 `constexpr std::string_view`**。`"true"` / `"false"` / `"null"` 之类反复出现的字面量用 `static constexpr std::string_view` 而不是 `std::string("true")`——前者零分配，后者每次构造都分配一次。

10. **Lexer 关键字查表用 `std::string_view` 比较**。`AYLexer.cpp::identifierType` 的 `unordered_map<std::string, TokenType>` 用 `std::string` 作为 key，每次 `find` 都要构造临时 `std::string`——应改成 `unordered_map<std::string_view, TokenType>`（key 为 `constexpr string_view`，源 lexeme 转 `string_view` 后查找）。

这十条不是建议，是闸门。每次 PR review 时，任何一条被违反必须立即指出并修复。

## 6.6 Compute declarations (Phase 2.5)

A `compute Name { ... }` block at the top level defines a GPGPU kernel (particle simulation, image processing, GPU culling, lightmap baking — anything that maps to a dispatch).

```phoskia
material PBR { vertex { } fragment { } }   // existing, unchanged

compute ParticleUpdate {                   // NEW, top-level
    let idx = 0
    return idx
}
```

**Compute is not a material.** There is no implicit output slot, no `in`/`out` semantic binding, no `gl_Position` / `gl_FragColor` analogue. A `return <expr>` in a compute body is **early-exit** (the thread has nothing to do) — it does NOT bind the return value to a fixed output.

The body is the same statement syntax as a `vertex` / `fragment` block: `let` / `return` / `if` / `for` / expression statements.

**Compute is emitted by the BGFX `.sc` backend in Phase 3.2.** A previous version of this section (Phase 2.5 era) stated that BGFX `.sc` does not support compute. That claim is **incorrect** as of bgfx 1.18 / shaderc 1.18:
- `bgfx::createProgram(ShaderHandle _csh, bool _destroyShader = false)` (overload at `bgfx.h:2704`) creates a compute program.
- `bgfx::dispatch(ProgramHandle _handle, ...)` (at `bgfx.h:1651`) submits a dispatch.
- `shaderc -f compute.sc -o out.bin --type compute --platform windows` accepts a GLSL compute source and produces a bgfx-compatible binary.
- Buffer flags `BGFX_BUFFER_COMPUTE_READ` / `BGFX_BUFFER_COMPUTE_READ_WRITE` (`bgfx.h:2257+`) bind storage buffers to compute stages.

The Phase 2.5 implementation in `src/AYBGFXConverter.cpp:521` carried a placeholder error "BGFX .sc does not support compute" that was correct at the time of the original research but stale now. Phase 3.2 removes that error and adds the actual compute emission path (see §11 Phase 3.2 row).

**Phase 3.2 — Compute 端到端落地 ✅** (2026-06-29):

- BGFX `.sc` compute emission — `convertComputeDecl` in `AYBGFXConverter` emits a single compute `.sc` source (see §6.6.1 for the exact emit shape).
- `shaderc --type compute` e2e test — `shaderc_compiles_compute_with_storage_buffer` in `Test_ShaderCompile.cpp` runs a storage-buffer + thread-id kernel through the full pipeline and asserts a non-empty `.bin` (which `bgfx::createProgram(_csh)` then accepts).
- Storage buffer declarations — `storage NAME : structuredbuffer<T>` (read) and `storage NAME : rwstructuredbuffer<T>` (read-write) in Phoskia surface syntax, mapped to GLSL `buffer Name { T data[]; } Name;` blocks (see §6.6.2).
- Thread-id builtins — `thread_id` / `group_id` / `dispatch_id` 0-arg calls return `vec3`, inlined to GLSL `gl_GlobalInvocationID` / `gl_WorkGroupID` / `(gl_NumWorkGroups * gl_WorkGroupID)` at emission time (see §6.6.3).

**Phase 3.2 fixes bundled in**:

- **MSVC SSO/NRVO bug, `BGFXConvertResult` flavour** — the same root cause as the Phase 3.2-pre `Compiler::compile` fix (see §6.8) showed up when `AYBGFXConverter::convertBGFX` returned `BGFXConvertResult` by value from a shaderc e2e test. The fix mirrors the Compiler pattern: added an out-param overload `void convertBGFX(const IRProgram&, BGFXConvertResult&)` and switched all test call sites. The return-by-value overload remains as a forwarder for callers that tolerate the risk.

**Out of scope (deferred to Phase 3.3+ / Phase 5+ 按需)**:

- HLSL emitter (`AYHLSLConverter`) — only justified if/when DXC-first quality is required or the project wants to drop shaderc.
- WGSL emitter (`AYWGLSConverter`) — only justified if/when the project targets WebGPU and wants native WGSL (bgfx does not currently have a WebGPU backend; this would require a runtime swap to wgpu-native / Dawn).
- HLSL `StructuredBuffer<T>` / `RWStructuredBuffer<T>` direct emission — comes with the HLSL emitter.
- `[numthreads(X, Y, Z)] compute Foo { ... }` attribute syntax — Phase 3.2 hard-codes `layout(local_size_x = 64) in;`. Per-decl numthreads is a Phase 3.3 surface-syntax extension.
- `groupshared` shared storage — Phase 3.3+; requires `BGFX_SHADER_LANGUAGE_GLSL` macro path through bgfx's `bgfx_compute.sh` (`SHARED shared` / `groupshared`).
- Custom struct types (`struct Particle { vec3 pos; vec3 vel; }`) as storage buffer element type — Phase 3.3; needs a parser-side `struct` decl + an IR `IRStructDecl` carrying field layout.
- Strict `uvec3` typing for thread-id — Phase 3.3; requires adding `uvec3` to `PrimitiveType` + `BuiltinTypes` + `lexemeToType` and propagating through `inferIdentifierExpr` / `emitExpr`.
- `uint` builtin type — Phase 3.3; same plumbing as `uvec3` (currently the storage buffer e2e uses `int` because the surrounding pipeline doesn't carry `uint` end-to-end).

### 6.6.1 BGFX compute `.sc` emit shape (Phase 3.2)

A Phoskia `compute Foo { ... }` declaration lowers to one `BGFXComputeFile { cs }` (single .sc source, no vs/fs/varyingdef split). The emitted source has the following shape:

```
$input                                          // empty (compute has no attributes)
$output                                         // empty (compute has no varyings)

#include "common.sh"

layout(local_size_x = 64) in;                   // numthreads fixed at 64; Phase 3.3+

buffer counters { int data[]; } counters;       // one per Storage-kind IRDeclaration
                                                // (Phase 3.2 emits even Read-only storage
                                                //  as `buffer`; the access field is
                                                //  preserved in IR for future HLSL)

void main()
{
    <body — IRComputeDecl::body verbatim>
}
```

The `void main()` body uses the same `emitStmt` machinery as material bodies, with `outputVar = nullptr` (no implicit output slot binding; `return` is early-exit only). `IRVariantAttribute` is silently skipped — compute bodies don't currently support kernel-level variants.

shaderc invocation: `shaderc -f cs_Foo.sc -o cs_Foo.bin --type compute --platform linux -p 430` (compute requires GLSL 4.30+; the material e2e tests use `-p 120`, which compute rejects).

### 6.6.2 Storage buffer declaration syntax (Phase 3.2)

```
storage NAME : structuredbuffer<ELEM>      // read access
storage NAME : rwstructuredbuffer<ELEM>    // read-write access
```

`ELEM` is a builtin scalar / vector lexeme: `float` / `int` / `vec2..4` / `ivec2..4` (`uint` is Phase 3.3 — see the out-of-scope list). Custom struct element types are Phase 3.3.

Both forms lower to the same GLSL `buffer Name { ELEM data[]; } Name;` block — GLSL doesn't distinguish read-only storage buffers at the source level (qualifiers live on the type, not the block). The access field is preserved in `IRDeclaration::storageAccess` so a future HLSL emitter can map `Read` → `StructuredBuffer<T>` and `ReadWrite` → `RWStructuredBuffer<T>`.

Element-type resolution: `IRGenerator::lowerDecl` calls `lexemeToType(st->elementType)` to carry the element type as a `Type` pointer (target-neutral). The BGFX converter emits `storageElementType->toString()` to get the GLSL lexeme; if `lexemeToType` returns `nullptr` (unrecognised lexeme), a warning is recorded and the emitter falls back to `vec4` — same fallback strategy as `PropertyDecl`.

### 6.6.3 Thread-id / group-id / dispatch_id builtins (Phase 3.2)

Three 0-arg builtin functions registered in `BuiltinFunctionRegistry::registerDefaults`, all returning `vec3`:

| Phoskia | GLSL | Notes |
|---|---|---|
| `thread_id()` | `gl_GlobalInvocationID` (uvec3) | Per-thread global linear index. Phoskia treats it as `vec3` so `.x` / `.y` / `.z` swizzles resolve to `float` and chain naturally with vector math. Strict `uvec3` is Phase 3.3. |
| `group_id()` | `gl_WorkGroupID` (uvec3) | Workgroup-space index. |
| `dispatch_id()` | `(gl_NumWorkGroups * gl_WorkGroupID)` (uvec3) | Dispatch-space workgroup index — emitted with surrounding parens so a subsequent `.x` swizzle lands on the product, not just `gl_NumWorkGroups`. |

Both call form (`thread_id()`) and bare-identifier form (`thread_id.x`) are accepted in Phoskia source. The bare-identifier form is the canonical one in the Phase 3.2 tests — `let idx = thread_id.x;`. Two pieces wire this end-to-end:

1. `TypeInference::inferIdentifierExpr` recognises bare `thread_id` / `group_id` / `dispatch_id` as 0-arg builtins and returns the return type (`vec3`) directly, so subsequent member access resolves correctly instead of seeing a `FunctionType` wrapper.
2. `AYBGFXConverter::emitExpr`'s `IRIdentifierExpr` branch inlines these three names to the corresponding GLSL builtin (rename-context-aware, so user variables named `thread_id` shadow the builtin).

The call-form (`thread_id()`) works because `inferCallExpr` already does `getFunctionByArity("thread_id", 0)` lookup; the inline happens in the `IRCallExpr` branch of `emitExpr`.

## 6.7 Phoskia IR (Phase 3.1)

**Motivation.** Before Phase 3.1, the BGFX backend consumed the Phoskia AST directly via manual `for`-loop + `dynamic_cast` (`src/AYBGFXConverter.cpp`, 891 lines). The next backend (HLSL / WGSL) would have had to duplicate that traversal. Phase 3.1 inserts a **target-neutral IR** between AST and backends so adding a backend is "implement one `IAYBackendConverter`" rather than "rewrite AST traversal from scratch".

**Shape.** The IR lives in `namespace ayt::shader::phoskia::ir` (see `include/AYIr.h`). It is a **1:1 mirror of the AST** with one key addition: every `IRExpr` carries a `std::shared_ptr<Type> resolvedType` populated at IR-generation time. The discriminator wrapper `IRDeclaration` unifies `UniformDecl` / `PropertyDecl` / `TextureDecl` into a single tagged struct (kind: `Uniform | Property | Texture`), each with its target-neutral fields (no GLSL lexeme strings leaking out of the AST). `SamplerKind { Sampler2D, Sampler3D, SamplerCube }` is the IR-side counterpart of the future `texture3d` / `textureCube` lexer keywords.

| AST (Phase 2) | IR (Phase 3.1) |
|---|---|
| `phoskia::Program` | `ir::IRProgram { materials, computes, warnings }` |
| `MaterialDecl` | `IRMaterialDecl { name, declarations, vertex, fragment }` (declarations pre-sorted; vertex/fragment are first-class fields) |
| `VertexFunc` / `FragmentFunc` | `IRVertexFunc` / `IRFragmentFunc` |
| `ComputeDecl` | `IRComputeDecl` (unchanged shape — Phase 3.2 HLSL backend implements it) |
| `UniformDecl { type: GLSL lexeme, name }` | `IRDeclaration { kind: Uniform, name, uniformType: Type ptr }` |
| `PropertyDecl { name, initializer }` | `IRDeclaration { kind: Property, name, propertyInit }` |
| `TextureDecl { name }` | `IRDeclaration { kind: Texture, name, samplerKind }` |
| `ShaderParam` | `IRShaderParam` |
| 7 Expr subclasses | 7 IRExpr subclasses + `resolvedType` |
| 7 body Stmt subclasses | 7 IRStmt subclasses (no resolvedType — only expressions have types) |

**Type resolution.** `IRGenerator::generate(ast, typeEnv)` (see `src/AYIr.cpp`) walks the AST exactly once. For each expression it sets `resolvedType` by:
1. **Identifier lookup** in the caller-supplied `TypeEnvironment` (when `SemanticAnalyzer` ran), or
2. **On-demand `TypeInference::infer`** as a graceful-degradation path when no env is supplied. The result is a `TypeVar` chain resolved to its concrete root via `resolveTypeVar`.

This eliminates the previous pattern where the BGFX converter re-ran `TypeInference` per LetStmt at emission time (the bug history at `b9723a5` for `vec3` property initializers was rooted in this re-inference cost).

**Why mirror, not SSA?** A previous TODO comment in `include/AYPhoskia.h` sketched an SSA-style instruction stream (`Load/Store/Add/Mul/Phi/BasicBlock`). SSA requires designing memory model, dominance frontiers, and phi placement — real work for an optimization pass. For backend emission, the mirror-IR with pre-resolved types is sufficient. If/when cross-backend optimization (constant folding, DCE, redundancy elimination) becomes a goal, an SSA layer can be added **on top of** this IR.

**What the IR does NOT do** (deferred to later phases):
- No SSA-style instruction stream / dominance frontiers / phi nodes (Phase 3.x optimization)
- No cross-backend optimization passes (constant folding, DCE) — requires SSA first
- No HLSL backend (Phase 3.2)
- No WGSL backend (Phase 3.3)
- No `storage` declarations inside `compute` (Phase 3.2 — comes with the HLSL backend that needs them)
- No backend-registry improvements (single-backend dispatch works fine today)

**Pipeline integration.** `Compiler::runPipeline` (in `src/AYPhoskia.cpp`) now inserts `IRGenerator::generate` between the parse / semantic phases and backend dispatch. The IR is always generated; backends consume the IR (the local `irProgram` in `runPipeline`) instead of `result.ast`. The Phoskia **surface language is unchanged** — IR is internal.

**API break.** `IAYBackendConverter::convert` now takes `const phoskia::ir::IRProgram& program` instead of `const phoskia::Program& ast`. The only implementer today (`AYBGFXConverter`) was retargeted; the behavior is byte-for-byte identical to the AST path (golden + shaderc e2e tests pass).

## 6.8 Compiler 返回值策略（SSO / NRVO 根因修复）

`Compiler::compile` 与 `Compiler::compileToBackend` 通过 **out 参数** 写回结果，不按值返回：

```cpp
void compile(const std::string& source, CompileResult& out);
void compileToBackend(const std::string& source, const std::string& backendName, CompileResult& out);
```

调用方模式：

```cpp
CompileResult result;
compiler.compile(src, result);
CHECK(result.success);
```

**为什么用 out 参数而不是 NRVO。** MSVC 长期存在 `std::string` SSO 与 NRVO 的交互 bug：当一个含 `std::string` 的 struct 跨过某个大小阈值，MSVC 会放弃具名返回值优化、按值拷贝 struct。拷贝过程中 SSO buffer 的 `_Myproxy` 指针被破坏，析构时 deref 到 `0xFFFFFFFFFFFFFFFF` 触发 AV。

`CompileResult` 自带 `std::string output`（组装后的 `.sc` 文本），天然命中此 bug。Phase 3.1 添加 `std::shared_ptr<ir::IRProgram>` 字段后，struct 跨过阈值，SSO 损坏第一次复现。最初的临时绕过是把 `result.ir` 删掉、把 IR 留在 `runPipeline` 局部变量里。但 Phase 3.2（HLSL 后端）/ 3.3（WGSL 后端）还会加 `std::string hlslSource` / `std::vector<uint32_t> dxil` 之类的 per-target 字段——任何后续字段都可能是另一个指针、再次越线。

out 参数形式完全消除了"按值返回"路径：struct 构造在调用方栈帧上，调用方有稳定的栈地址（不是 SSO proxy carrier），pipeline 一边执行一边按字段填入。没有 NRVO 可言，没有跨 struct 的 SSO string 拷贝。

同样的先例在多年前 `Compiler::tokenize` / `Lexer::tokenize` 上就出现过——`std::vector<Token>` 也用 out 参数避免同一个 MSVC 问题（`include/AYPhoskia.h:82-83` 注释）。

**Phase 3.2+ 加字段现在是安全的。** HLSL / WGSL 后端可以放心地往 `CompileResult` 加 `std::vector<uint32_t> dxil` / `std::string hlslSource` / `std::vector<uint8_t> spirv` 等，不会再踩到 SSO bug。代价仅是 `runPipeline` 入口处一次 `out = CompileResult{};` 复位，淹没在 lex/parse/IR-gen 后面。

**已删除的死代码**：
- `CompileResult::typeEnv`（从未写入、从未读取）
- `Compiler::errors()` / `Compiler::hasErrors()`（零调用方，与 `out.errors` 重复）
- 便捷自由函数 `inline compile(src)`（零调用方）

### 6.7 Phase 3.4 — UBO（Uniform Buffer Object）

#### 6.7.1 表面语法

```phoskia
// 顶层（与 material / compute 同级）
uniformblock Camera {
    vec3 position
    vec3 direction
    float fov
}
```

GLSL emit：
```glsl
layout(std140, binding = 0) uniform Camera {
    vec3 position;
    vec3 direction;
    float fov;
} Camera;
```

字段访问走 `MemberExpr` 路径（`Camera.position` 跟 `texture2D.field` 同形），emit 路径透明 —— 不需要特殊化。

#### 6.7.2 IR 形状

`IRProgram` 加 `std::vector<std::unique_ptr<IRDeclaration>> uniformBlocks;`。每个 entry 是 `IRDeclaration { kind=UniformBlock, name, uboFields: vector<shared_ptr<Type>>, uboFieldNames: vector<string>, uboBinding: int }`。

`IRGenerator` 内 `nextBinding_` 计数器：~~（**Phase 3.5-B 已删除**）~~ — 改为透传 AST 的 `binding` 字段（-1 或字面量）。Binding slot 决议下放到 `convertBGFX` 顶层（per-program 重复检测 + auto from `max(explicit)+1`）。

`lowerDecl` 处理 `UniformBlockDecl` —— 复用 `lexemeToType` 解析字段 type（与 StorageDecl / SharedDecl 同一张表），未识别的 lexeme warn + fallback vec4。

#### 6.7.3 emit 形状

`convertBGFX` 顶层一次性 emit 全部 UBO decls 到 `_uboDecls` 字符串。`convertMaterial` 把 `_uboDecls` 拼到 vs/fs 头（`#include "common.sh"` 之后）。`convertComputeDecl` 把同一字符串拼到 cs 头。

GLSL 允许同一个 `uniform Name { ... } Name;` 在多个 stage 出现，compiler 自动 dedupe。`std140` layout 由 GLSL compiler 计算（Phoskia 端不做 sizeof/alignment —— HLSL cbuffer packoffset 才有需要，Phase 5+）。

#### 6.7.4 已知 limitation

UBO 块名（`Camera`）和字段（`Camera.position`）**不注册**到 body 的 `TypeEnvironment`。所以 `let p = Camera.position` 在 IR 层推断为 fresh TypeVar（emit 不带 GLSL 类型前缀），shaderc 端做类型检查。完整 struct 推断留 Phase 4+。

#### 6.7.5 shaderc profile 升级

`binding = N` 语法要求 GLSL 4.30+。Phase 3.4 把 material / compute 整个 e2e 套件从 `-p 120` 升到 `-p 430`（6 个 `Test_ShaderCompile.cpp` 站点）。`-p 430` 的 `layout(std140)` block 语法 + `gl_GlobalInvocationID` 全部向后兼容 Phase 3.2 黄金输出；`unittest/golden/*.sc` 字节级不变。

#### 6.7.6 Out of scope

- struct 字段（依赖 struct 类型系统，Phase 3.3 跳过）
- 嵌套 UBO（一个 UBO 字段是另一个 UBO）
- ~~用户显式 `binding = N`（编译器自动分配；用户覆盖留 Phase 4+）~~ **Phase 3.5-B 已实现** — 详见 §6.7.8
- ~~storage decl 的 `binding = N` 语法（UBO-first；storage 留后续 phase）~~ **Phase 3.5-A 已实现** — 详见 §6.7.7
- HLSL `cbuffer` emit + packoffset layout（Phase 5+）
- WGSL `@group(0) @binding(0) var<uniform>` 概念映射（Phase 5+ WGSL emitter）

### 6.7.7 Phase 3.5-A — Storage binding 表面语法

#### 表面语法

```phoskia
compute Foo {
    storage inputs  : structuredbuffer<float>   binding 0
    storage outputs : rwstructuredbuffer<int>   binding 1
    storage debug   : rwstructuredbuffer<int>             // 自动 binding = 2

    let idx = thread_id.x
    let v = inputs[idx]
    outputs[idx] = int(v) + 1
    debug[idx] = 0
}
```

可选 `binding <non-negative-int>` 后缀（关键字 `Binding` + 字面 int）。无 binding 的 decl 走"自动分配 slot"路径，从 `max(显式 binding)+1` 起始，避免与显式 slot 撞。

#### emit 形状

```glsl
layout(std430, binding = 0) buffer inputs { float data[]; } inputs;
layout(std430, binding = 1) buffer outputs { int data[]; } outputs;
layout(std430, binding = 2) buffer debug { int data[]; } debug;
```

`std430` 而不是 `std140`——storage buffer 的 std430 layout 规则对 runtime-sized 数组友好（looser packing）。

#### IR 形状

`IRDeclaration::storageBinding`（int）：`-1` = 无显式 binding（BGFX 自动分配）；`>= 0` = 用户写的字面 slot。

#### 重复 binding 检测

emit 时（`AYBGFXConverter::convertComputeDecl` 入口）扫一遍 compute.declarations：

```cpp
std::unordered_map<int, std::string> usedBindings;
for (auto& decl : compute.declarations) {
    if (decl->storageBinding < 0) continue;
    auto it = usedBindings.find(decl->storageBinding);
    if (it != usedBindings.end()) {
        throw runtime_error("Storage buffer '" + decl->name +
            "' has duplicate binding " + ...);
    }
    usedBindings[decl->storageBinding] = decl->name;
}
```

错误冒泡到 `BGFXConvertResult::errors`，frontend 通过 `CompileResult::errors` 暴露。

#### shaderc profile

`layout(std430, binding = N)` 需要 GLSL 4.30+——shaderc 调用继续用 `-p 430`（Phase 3.4 已升过）。本块无需再改 shaderc 调用。

#### 已知 limitation

- 多 compute 共享 binding slot 不做跨 compute 校验——`bgfx::setUniform(handle, ptr, sizeof(buffer))` 在每个 compute 的 dispatch 上独立设置，frontend 责任保证不撞。本编译器只在**同一 compute 内**检测重复。
- shaderc e2e 在 Linux GLSL 430 已验证 UBO + SSBO 都支持（独立 fixture 编译通过）。

### 6.7.8 Phase 3.5-B — UBO binding 表面语法

#### 表面语法

```phoskia
// 顶层（与 material / compute 同级）
uniformblock Camera {
    vec3 position
    float fov
}
uniformblock Lighting {
    vec3 ambient
    uint flags
} binding 3
uniformblock ToneMap {        // 自动 binding = max(3)+1 = 4
    vec3 gain
}
```

可选 `binding <non-negative-int>` 后缀。**完全复用 Phase 3.5-A 关键字 `Binding`**（无新词法工作）。无 binding 的 decl 走"自动分配 slot"路径，从 `max(显式 binding)+1` 起始。`}` 后的尾 `;` **可选**（Python-like 不破坏现有 UBO 源）。

#### emit 形状

```glsl
layout(std140, binding = 0) uniform Camera { ... } Camera;
layout(std140, binding = 3) uniform Lighting { ... } Lighting;
layout(std140, binding = 4) uniform ToneMap { ... } ToneMap;
```

`std140`（**不是** SSBO 的 std430）——UBO 块是固定大小、需要严格 padding。

#### IR 形状

`IRDeclaration::uboBinding`（int）：`-1` = 无显式 binding（BGFX 自动分配）；`>= 0` = 用户写的字面 slot。

**Phase 3.5-B 相对 Phase 3.4 的 IR 变化**：删除 `IRGenerator::nextBinding_` 计数器（§6.7.2 的旧描述）。IR generator 改为**纯透传** AST 的 `binding` 字段（-1 或字面量）；binding 决议全部下放到 BGFX emit。这跟 §6.7.7 的 SSBO 走完全一样的路径，去掉了特化的 UBO counter。

#### 重复 binding 检测

emit 时（`AYBGFXConverter::convertBGFX` 入口）扫一遍 `program.uniformBlocks`：

```cpp
std::unordered_map<int, std::string> usedBindings;
for (auto& ub : program.uniformBlocks) {
    if (ub->uboBinding < 0) continue;
    auto it = usedBindings.find(ub->uboBinding);
    if (it != usedBindings.end()) {
        errors.push_back("UniformBlock '" + ub->name +
            "' has duplicate binding " + ...);
    }
    usedBindings[ub->uboBinding] = ub->name;
}
if (any duplicates) { out.success = false; return; }  // 早返不回 emit
```

**作用域 per-program**（不是 per-compute）——UBO 在 vs/fs/cs 间共享，全程序单一 namespace。SSBO 的 per-compute 作用域是因为 storage decl 嵌在 `IRComputeDecl::declarations` 里；UBO 在 `IRProgram::uniformBlocks` flat vector，per-program 是天然边界。

#### shaderc profile

无变化——`layout(std140, binding = N)` 是 GLSL 4.30 标准语法，Phase 3.4 已升过 `-p 430`。

#### 与 Phase 3.5-A 对比

| 维度 | Phase 3.5-A (SSBO) | Phase 3.5-B (UBO) |
|---|---|---|
| Decl 语法 | `storage X : rwstructuredbuffer<T> binding N;` | `uniformblock X { ... } binding N;` |
| layout qualifier | `std430` | `std140` |
| BGFX emit 入口 | `convertComputeDecl`（每 compute） | `convertBGFX`（每 program） |
| Duplicate 作用域 | per-compute | **per-program**（UBO 全局）|
| IR 起始状态 | 已经是 -1 透传（无需 counter） | 3.5-B 删除 `nextBinding_` 后变成 -1 透传 |
| 测试增量 | +12 | +20 |

#### 已知 limitation

- 多 UBO 共享 binding slot 不做跨 shader stage 边界校验——本编译器在**整个 program** 上检测重复；如果同一个 shader 在 host 端跨 stage bound（`bgfx::setUniform` 在 vs/fs 分别调用），仍然按 program 唯一性保护。
- UBO 块名 + 字段类型不注册到 body 的 TypeEnvironment——`let p = Camera.position` 推断为 fresh TypeVar，emit 透明，shaderc 端做类型检查。完整 struct 推断留 Phase 4+ 跟 `struct` 类型系统一起做。

## 7. 改语法的"链路"

Phoskia 的语法控制在以下 5 个文件里。**改一个语法特性需要同步修改这一组文件**：

| 改什么 | 必改文件 | 可选改 |
|---|---|---|
| **重命名关键字** | `AYToken.h`（enum）、`AYLexer.cpp`（关键字表）、`AYParser.cpp`（分支） | `design.md` |
| **加新关键字** | 同上 3 个 | `AYAst.h`（如果引入新节点） |
| **加新语句**（while/break/continue） | `AYToken.h`、`AYLexer.cpp`、`AYAst.h`（新节点 + AstVisitor::visit 重载）、`AYParser.cpp`（parseXxx + 分支）、`AYBGFXConverter.cpp`（生成 .sc） | `design.md` |
| **加新类型**（mat2x3 / half） | `AYToken.h`、`AYLexer.cpp`、`AYType.h/cpp`（BuiltinTypes）、`AYBGFXConverter.cpp`（类型映射到 GLSL） | `AYBuiltinFunctions.cpp`（如果新增类型相关的数学函数） |
| **加新声明**（function/const） | `AYToken.h`、`AYLexer.cpp`、`AYAst.h`、`AYParser.cpp`、`AYBGFXConverter.cpp`、`design.md` | — |
| **改 shading 语义**（加 vertex/fragment 区分） | `AYToken.h`、`AYLexer.cpp`、`AYAst.h`（ShadingFunc 拆分）、`AYParser.cpp`、`AYBGFXConverter.cpp`、`design.md` | `AYShaderProgram.h`（uniform binding） |
| **改运算符优先级** | `AYParser.cpp`（getPrecedence switch）、`design.md` | — |
| **加新后端**（HLSL/WGSL） | 新建 `AYHLSLConverter.h/.cpp`（实现 `IAYBackendConverter`）、`AYPhoskia.cpp`（注册） | `IAYBackendConverter.h`（如果增加 Platform 枚举） |

**改语法的标准流程**（推荐）：

1. **改 `design.md` 第 10 节 BNF** —— 先写人类可读定义
2. **改 `AYToken.h` 的 `TokenType` enum** —— 加新关键字
3. **改 `AYLexer.cpp` 的 `identifierType`** —— 加关键字到查表
4. **改 `AYAst.h`** —— 加新节点 + 在 `AstVisitor` 加 `visit` 重载
5. **改 `AYParser.cpp`** —— 加 `parseXxx` 方法 + 在 `parseStatement` 加分支
6. **改 `AYBGFXConverter.cpp`** —— 加节点到 `.sc` 的生成分支
7. **加单元测试**（Phase 2+）—— 确保 Lexer 识别新关键字、Parser 解析新语法
8. **更新 `design.md` 第 6 节语法详解** —— 文档同步

**改语法的成本梯度**：

- 重命名关键字：~5 个文件，< 1 小时
- 加新关键字：~3 个文件，< 1 小时
- 加新语句：~5-7 个文件，2-4 小时
- 加新类型：~5-7 个文件，半天
- 加新后端：~1 个新文件 + 注册，半天到 1 天

## 8. 后端与多平台（Phase 2+）

### 8.1 BGFX 三件套（vs / fs / varying.def.sc）

每个 Phoskia material 经 `AYBGFXConverter` 转换后产出**三个 .sc 源码文件**，由 `shaderc` 单独编译为 `.bin`：

| 文件 | 内容 | shaderc 调用 |
|---|---|---|
| `vs_<Material>.sc` | vertex shader（含 `$input` / `$output` / uniforms / body） | `--type vertex` |
| `fs_<Material>.sc` | fragment shader（含 `$input` / uniforms / textures / body） | `--type fragment --varyingdef varying.def.sc` |
| `varying.def.sc` | varying/attribute 的 bgfx semantic binding（POS / NORMAL / COLOR0 / TEXCOORD0 等） | 被 shaderc 引用 |

**bgfx semantic 映射（converter 内部硬编码表）**：

| Phoskia 关键字 | bgfx semantic | 类型 | `a_*` 名字 | `v_*` 名字 |
|---|---|---|---|---|
| `position` | POSITION | `vec3` | `a_position` | `v_position` |
| `normal`   | NORMAL   | `vec3` | `a_normal`   | `v_normal`   |
| `color`    | COLOR0   | `vec4` | `a_color0`   | `v_color0`   |
| `texcoord` | TEXCOORD0| `vec2` | `a_texcoord0`| `v_texcoord0`|

`shaderc` 不识别 `[section]` 风格的 marker——所有 metadata 都通过 `$input` / `$output` 指令（从 Phoskia 的 `in`/`out` 声明转换）和 `varying.def.sc` 表达。

### 8.2 BGFXShaderType（保留用于 compile 调度）

```cpp
enum class BGFXShaderType {
    Vertex,   // converter 隐式绑定 `return <expr>` → gl_Position
    Fragment, // converter 隐式绑定 `return <expr>` → gl_FragColor
    Compute,  // Phase 2+ — 无固定输出变量，raw dispatch
    Ray       // Phase 3+ — 独立路径
};
```

**Compute shader 处理**：BGFX `.sc` 通过 `shaderc --type compute` 直接支持 compute（Phase 3.2 ✅）。运行时消费 `.bin` 的入口：
- `bgfx::createProgram(ShaderHandle _csh, bool _destroyShader = false)`（`bgfx.h:2704` 重载）— 创建 compute program
- `bgfx::dispatch(ProgramHandle _handle, uint32_t _numGroupsX, uint32_t _numGroupsY, uint32_t _numGroupsZ)`（`bgfx.h:1651`）— 提交 dispatch
- Storage buffer 绑定：`BGFX_BUFFER_COMPUTE_READ` / `BGFX_BUFFER_COMPUTE_READ_WRITE` flag（`bgfx.h:2257+`）
- shaderc 调用约定：`shaderc -f cs_Foo.sc -o cs_Foo.bin --type compute --platform <plat> -p 430`（compute 要求 GLSL 4.30+，material 用的 -p 120 会被拒绝）

`compute Name { ... }` 是顶层声明（与 `material` 平级），定义 GPGPU kernel。Body 语法同 material：let / return / if / for / expression statements。无 in/out 语义绑定、无 implicit output slot、`return <expr>` 是 early-exit（不绑到输出）。详见 §6.6 + §6.6.1 + §6.6.2 + §6.6.3。

Phase 3.2 之前（Phase 2.5 时代）的 `BGFX .sc does not support compute` placeholder 错误已删除。HLSL / WGSL emitter 仍属 Phase 5+ 按需启动。

**长期路线**：

| Shader 类型 | 现状 | Phase 2 | Phase 3 |
|---|---|---|---|
| Vertex | converter 隐式绑定 `return <expr>` → `gl_Position` | 完善 `gl_Position` 语义 | 多后端 |
| Fragment | converter 隐式绑定 `return <expr>` → `gl_FragColor` | 完善 PBR/光照 | 多后端 |
| Compute | BGFX `.sc` via `shaderc --type compute` (Phase 3.2 ✅) | DX11/HLSL compute 生成 (Phase 5+ 按需) | SPIR-V / WGSL (Phase 5+ 按需) |
| Ray | 不支持 | — | 独立架构 |

### 8.3 shaderc 集成方式（路线对比）

`AYBGFXConverter` 只产出 `.sc` 文本。**从 `.sc` 到 `.bin` 的 shader 编译**有三条路线，复杂度与解耦程度递增：

**路线 A — 外部 shaderc.exe（Phase 1 采用）**

`bgfx` 发布的独立 CLI 工具（`thirdParty/bgfx-install/<config>/bin/shaderc.exe`）。调用方（游戏构建脚本 / CI / 工具）spawn 进程传 `--type` / `--platform` / `-p` 等参数。

- ✅ 解耦：AYShader 不依赖 shaderc 工具链，shader 编译可以独立升级 bgfx 版本
- ✅ 零额外链接：shaderc.exe 单独 vendored ~2 MB
- ✅ bgfx 官方分发，已处理所有 transitive deps（DXC、glslang、glsl-optimizer、Metal tools）
- ❌ spawn 进程开销（通常仅 build / load time，不在 hot path）
- ❌ Windows 上 Windows ↔ POSIX 路径转换坑

**路线 B — 链接 shaderc C++ API（已具备，但 Phase 1 未采用）**

`bgfx/tools/shaderc/shaderc.h` 暴露 in-process API：

```cpp
#include "shaderc.h"  // bgfx/tools/shaderc/

bgfx::Options opts;
opts.platform = "windows";
opts.profile = "120";
opts.shaderType = 'v';  // vertex
opts.inputFilePath = "...";

bgfx::compileGLSLShader(opts, /*version*/0, codeString, &writer, &msgWriter);
```

可直接调 `bgfx::compileGLSLShader` / `compileHLSLShader` / `compileMetalShader` / `compileSPIRVShader` / `compileDxilShader` / `compileWgslShader`，写到 `bx::WriterI`（内存 buffer 即可）。

- ✅ in-process，无 spawn 开销
- ✅ 编译错误可以直接以 `CompilerError` 形式上报
- ❌ 需链接 `bx` + `glsl-optimizer` + 可选 `glslang` / DXC 动态库
- ❌ 拉进 ~30+ 头文件 + glsl-optimizer 编译产物（即使不用 GLSL 优化也要链接符号）
- ❌ bgfx 升级时 shaderc 内部 ABI 变动需要同步

**路线 C — 多 backend 直调原生 API（Phase 3 目标）**

绕开 bgfx 的 `.sc` 抽象，每个 backend 直接调平台 native compiler：

| Backend | 编译入口 |
|---|---|
| DX11 | `D3DCompile()` (d3dcompiler.dll) |
| DX12 | DXC (`DxcCreateInstance`) |
| Vulkan | glslang + SPIRV-Tools → SPIR-V bytecode |
| Metal | `metal` CLI → metallib / 直接调 Metal API |
| WebGPU | naga / tint → WGSL |

- ✅ 极致性能（DXC 编译比 glslang → DXC 两步少一次 IR 翻译）
- ✅ 摆脱 bgfx `.sc` 抽象（部分平台已不推荐 .sc）
- ❌ 工作量大：每 backend 独立代码、独立的 attribute/varying 处理
- ❌ 跨 backend 的 type/builtin 同步成本

**Phase 1 决策与未来迁移**

- Phase 1 / Phase 2 走路线 A。`Test_ShaderCompile` 已用 `_popen` 验证跨平台矩阵（windows / linux / osx / android / ios / asm.js / orbis × GLSL 1.20 / ESSL 3.20 / Metal / SPIR-V 大部分组合）。
- 路线 B 升级门槛低（仅需把 `thirdParty/bgfx/tools/shaderc/` + `bx/` 纳入 CMake），适合"想脱 spawn 但仍用 bgfx 编译栈"的中间阶段。
- 路线 C 是 Phase 3 长期目标。

### 8.4 Binary 输出 API — `.sc` 作为内部中间产物（Phase 3.6 提案）

**问题**：现状（Phase 3.5 及之前）用户调用 `Phoskia::compile(src)` 后拿到 `CompileResult { .success, .output: std::string, .errors, .uniforms, .textures }`。其中 `.output` 是个**拼接的 .sc 文本**（`// === material 0 vs === ...` + `// === material 0 fs === ...` 加上 `// === compute 0 cs === ...`）。frontend 必须自己 `string -> filesystem` → 再 spawn `shaderc.exe -f x.sc -o x.bin` → 再 wire binary 到 bgfx。

这意味着 frontend **看得到 `.sc`** —— 一个本来应该藏在 backend 内部的中间产物。用户视角：

```
Phoskia (源码)  ── compile() ──>  .sc (frontend 必须存盘 + 调 shaderc)
                                    │
                                    └─ shaderc.exe ──>  .bin (frontend 拿到)
```

跟理想模型差距明显：

```
Phoskia (源码)  ── compile() ──>  .bin (frontend 拿到)
```

`.sc` 是**必须存在**的中间产物（shaderc 输入要求），但**不应该出现在 frontend 视野里**。

**Phase 3.6 目标**：把 shaderc 调用内化进 AYBGFXConverter，frontend 一次调用直接拿到 `.bin` bytes。`.sc` 在 debug 模式（`AY_PHOSKIA_DUMP_SC=1`）才落盘，平时永远只在内存。

**API 形状**：

```cpp
// 当前（Phase 3.5）frontend 视角
Phoskia phoskia;
auto result = phoskia.compile(src);
//   result.output 是 .sc 文本，frontend 自己存盘调 shaderc
//   result.uniforms / textures 是 binding 元数据（保留）

// Phase 3.6 frontend 视角（默认路径：拿 .bin）
Phoskia phoskia;
auto program = phoskia.compileToProgram(src);  // 新方法
//   program.vsBin / fsBin / csBin : std::vector<uint8_t>  (frontend 直接喂 bgfx::createShader)
//   program.uniformBlocks / storageBuffers / uniforms / textures：binding 元数据（保留）
//   不再有 .sc 字串出现在 frontend
```

**Debug 路径：拿 `.sc` 在内存里（用户 2026-06-30 sign-off — 调试非常必要）**

`.sc` 是 backend 内部产物，**默认不暴露**。但 debug 时不可避免要查（diff vs 上一次、错误排查、shader playground 复现）。提供两种 debug 入口，**两个正交**：

```cpp
// Debug 入口 A：直接在内存里看 .sc（不落盘）
CompileOptions opts;
opts.keepSources = true;
auto program = phoskia.compileToProgram(src, opts);
//   program.vsBin / fsBin / csBin  ← 同上，binary 也填了
//   program.sources                 ← std::map<std::string, std::string>
//        { "vs_PbrShader.sc": "...", "fs_PbrShader.sc": "...", "cs_Foo.sc": "..." }
//   是 backend emit 出来的**完全相同**的字符串（byte-equal to what shaderc saw）

// Debug 入口 B：落盘到 temp dir（与 A 独立）
CompileOptions opts;
opts.dumpIntermediate = true;
opts.dumpDir = "D:/debug-shader-out";   // default = tempdir()/phoskia-sc/
auto program = phoskia.compileToProgram(src, opts);
//   .sc 写到 dumpDir 下；program.sources 仍空（落盘 ≠ 内存保留）
```

**环境变量快捷开关**（与 CompileOptions 等价，独立存在是为了脚本 / IDE 启动时一行加）：

```cpp
// AY_PHOSKIA_KEEP_SOURCES=1   → 全局 keepSources = true
// AY_PHOSKIA_DUMP_SC=1       → 全局 dumpIntermediate = true
```

两者并存 / 任选其一 / 全关 — 都合法。

**为什么 `program.sources` 是 `std::map<std::string, std::string>`**：
- 键是 backend 内部标识（`"vs_<Material>.sc"`、`"cs_<Compute>.sc"` 等）。frontend 不应该 pin 这些名字（任何 backend 重构都可能变）
- 值是 `.sc` 的完整 GLSL 文本（多行 string with `#include "common.sh"` 和 layout decl）
- 用 map 而非 vector —— frontend 可以直接 `program.sources["vs_PbrShader.sc"]` 按字符串查，不用记 index

**API surface 契约**：
- `BGFXProgram::sources` 默认 fill（empty map）；只有 `keepSources == true` 才 non-empty
- `BGFXProgram::sources` 的内容跟 backend emit 的 .sc 是 byte-equal；这意味着 frontend 可以把 `program.sources[k]` 写盘再 spawn shaderc —— 跟 Phase 3.5 行为一致但前端不需要 spawn
- `BGFXProgram::sources` 是**只读快照**：Phase 3.6 不暴露 streaming / lazy API

**Backend 内部细节**（用户看不到）：

```
AYBGFXConverter::convertBGFX(program)
    ↓ emit .sc 字符串（内存中）
    ↓ AYShadercDriver::compileToBytes(scStr, type, platform, profile)
    ↓ 沙盒 spawn shaderc.exe（路线 A）OR in-process (路线 B; 仍选定路线 A：解耦 + 零额外链接)
    ↓ 返回 .bin bytes
```

**Phase 3.6 边界**：

| 在 Phase 3.6 | 不在 Phase 3.6 |
|---|---|
| `.sc` 字符串从 `BGFXConvertResult` 公开字段移走 | bgfx::createShader / createProgram 调用 |
| shaderc 调用内化进 `AYBGFXConverter::compileToBinary()` | bgfx wire-up（frontend 拿 bin 后调 bgfx API） |
| `AY_PHOSKIA_DUMP_SC=1` 控制 .sc 落盘 | 与具体 frontend (AYRenderer) 集成 |
| 测试 plumbing 从 "手工写 .sc 盘 → spawn shaderc 读 .sc" 改为 "emit in-memory → driver 调 shaderc" | WGSL / HLSL backend 的 binary 输出 |

**为什么 Phase 3.6 仍走路线 A**（spawn shaderc.exe 而不是 in-process）：

- Phase 1 / Phase 2 已写好 spawn plumbing（Test_ShaderCompile 验过跨平台），内化只是把 plumbing 包进 backend
- 路线 B（in-process）需要拉 bx + glsl-optimizer + DXC/glslang 动态库，~30+ 头文件污染 — 等真有 hot-path 性能需求再考虑
- 路线 C（每个 backend 直调原生 API）独立 Phase 5+ 工作

**Frontend 双模接口**：

```cpp
struct CompileOptions {
    // ... 已有 fields
    bool keepSources = false;          // 显式 opt-in 才把 .sc 字符串保留到 program.sources
    bool dumpIntermediate = false;     // 显式 opt-in 才把 .sc 写到 dumpDir
    std::string dumpDir;               // 仅在 dumpIntermediate=true 时用，默认 = tempdir()/phoskia-sc/
};

Phoskia phoskia;
auto program = phoskia.compileToProgram(src, opts);
// program.vsBin / fsBin / csBin 总是 fill（成功时）
// program.sources 在 keepSources=false 时为空 map
// 落盘仅在 dumpIntermediate=true 时发生
// 两个开关正交，可以单独 / 一起 / 都不开
```

**测试 hygiene**：当前 `Test_ShaderCompile` 把 .sc 写盘 → spawn shaderc 读盘的模式重构后是"内调 `AYShadercDriver` 直接编 `.sc` → 拿 `.bin`"，少一个 IO 层。

**Phase 3.6 step list**（§14.3 维护）：

| Block | 范围 | 估算 |
|---|---|---|
| 3.6-A | **design.md** §8.4（本文档，本节） | 0.25 天 ✅ |
| 3.6-B | `AYShadercDriver` 抽象 + `AYBGFXConverter::compileToBinary()` 内化 shaderc 调用；frontend `compileToProgram()` 暴露 binary API；`.sc` 字段从 `BGFXConvertResult` 公开移到 private（debug dump 走 `opts.dumpIntermediate`） | 1.5-2 天 |
| 3.6-C | Test_ShaderCompile 重 plumbing 用 `AYShadercDriver`（少一层 IO） | 1 天 |
| 3.6-D | `AY_PHOSKIA_DUMP_SC=1` env 解析 + 实现 + unit tests | 0.25 天 |
| **总计** | **3-3.5 天** | — |

**验收**：
- 默认路径：frontend 写 `phoskia.compileToProgram(src).vsBin` 一行拿到 binary，完全不知道 `.sc` 存在过。
- Debug 路径 A（in-memory）：`opts.keepSources = true` 让 `program.sources` 拿到 in-memory `.sc`，**不**落盘
- Debug 路径 B（落盘）：`opts.dumpIntermediate = true` 或 env `AY_PHOSKIA_DUMP_SC=1` 落盘 `.sc` 到 temp dir
- 两个 debug 入口正交 — 都开 / 单开 / 都不开 都合法


### 新增后端步骤

1. 实现 `IAYBackendConverter` 接口
2. 构造 `Compiler` 后调用 `registerBackend("hlsl", ...)` 即可
3. 编译时通过 `compileToBackend(source, "hlsl")` 选择

`Platform` 枚举已预定义 `DX11/DX12/OpenGL/Vulkan/WebGPU/BGFX`，新增平台无需改枚举。

## 9. 资源存储与缓存（Phase 2+）

Phase 1 不实现缓存。Phase 2 引入 `AYShaderCache`（已存在类骨架），流程：

```
首次运行
  ↓
包内 Phoskia 源码
  ↓
检查本地缓存 shader_cache/{platform}/Material.bin
  ├── 有 → 比较源码哈希
  │       ├── 匹配 → 跳过编译，直接加载
  │       └── 不匹配 → 重新编译
  └── 没有 → 完整编译，缓存
```

## 10. Phoskia 完整语法 BNF

```bnf
<program>           ::= <top_level_decl>*

<top_level_decl>    ::= <material_decl>
                      | <compute_decl>

<material_decl_list> ::= <material_decl>
                       | <material_decl_list> <material_decl>

<material_decl>     ::= "material" <identifier> "{" <declaration_list> "}"

<compute_decl>      ::= "compute" <identifier> "{" <statement_list> "}"
                      ; body 含 storage buffer 声明（Phase 3.2）：
                      ;   storage NAME : structuredbuffer<T>
                      ;   storage NAME : rwstructuredbuffer<T>
                      ; 与 let / return / if / for / expression 自由混合。

<declaration_list>   ::= <declaration>
                       | <declaration_list> <declaration>

<declaration>       ::= <property_decl>
                      | <uniform_decl>
                      | <storage_decl>          ; Phase 3.2 — 仅在 compute body 内有意义
                      | <texture_decl>
                      | <sampler_decl>
                      | <vertex_func>
                      | <fragment_func>
                      | <variant_attribute>

<storage_decl>      ::= "storage" <identifier> ":" <storage_kind> "<" <type> ">" ";"
                      ; <storage_kind> ::= "structuredbuffer" | "rwstructuredbuffer"
                      ; <type> 见 <type> 规则。Phase 3.2 限制为 builtin
                      ; scalar / vector（float / int / vec2..4 / ivec2..4），
                      ; uint / 自定义 struct 留 Phase 3.3。
                      ; Read 与 ReadWrite 在 GLSL 路径下 emit 形态相同
                      ; (`buffer Name { T data[]; } Name;`)；access 字段
                      ; 在 IR 上保留供未来 HLSL emitter 区分
                      ; StructuredBuffer<T> vs RWStructuredBuffer<T>。

<variant_attribute> ::= "[" "variant" <identifier> "]"
                      ; Phoskia 源码用纯 [variant ...] 形式（无 # 前缀），
                      ; 看起来更高级。Lexer 切出 LeftBracket 作为普通 token；
                      ; parser 在 expression 上下文里通过 lookahead
                      ; 区分数组下标与 variant attribute（见 §6.3）。
                      ; BGFX 后端产出: 在该节点后续所有 statement 之前写入
                      ;   #ifndef BGFX_VARIANT_<UPPERCASE(name)>
                      ; 在该 block 末尾或下一个 variant_attribute 之前写入
                      ;   #else
                      ;   <statement>*
                      ;   #endif
                      ;（name 中非字母数字字符替换为下划线；与 shaderc 的
                      ;  --define BGFX_VARIANT_<NAME> 联动使用）

<property_decl>     ::= "property" <identifier> "=" <expression> ";"

<uniform_decl>      ::= "uniform" <type> <identifier> ";"

<texture_decl>      ::= "texture2d" <identifier> ";"

<sampler_decl>      ::= "sampler" <identifier> "=" <expression> ";"

<vertex_func>       ::= "vertex" "{" <param_decl_list> <statement_list> "}"

<fragment_func>     ::= "fragment" "{" <param_decl_list> <statement_list> "}"

<param_decl_list>   ::= <param_decl>*
                       | <param_decl>* <statement_list>   ; first statement may follow param decls without separator

<param_decl>        ::= <io_keyword> <identifier> ":" <phoskia_semantic> [ "=" <expression> ]

<io_keyword>        ::= "in"
                      | "out"

<phoskia_semantic>  ::= "position" | "normal" | "color" | "texcoord"

<statement_list>    ::= <statement>
                       | <statement_list> <statement>

<statement>         ::= <let_stmt>
                      | <assignment_stmt>
                      | <return_stmt>
                      | <if_stmt>
                      | <for_stmt>
                      | <expression_stmt>

<let_stmt>          ::= "let" <identifier> "=" <expression> ";"

<assignment_stmt>   ::= <identifier> "=" <expression> ";"

<return_stmt>       ::= "return" <expression> ";"

<if_stmt>           ::= "if" "(" <expression> ")" "{" <statement_list> "}"
                      | "if" "(" <expression> ")" "{" <statement_list> "}"
                        "else" "{" <statement_list> "}"

<for_stmt>          ::= "for" "(" <identifier> "in" <expression> ")" "{"
                        <statement_list> "}"

<expression_stmt>   ::= <expression> ";"

<type>              ::= "float"
                      | "vec2" | "vec3" | "vec4"
                      | "int" | "ivec2" | "ivec3" | "ivec4"
                      | "mat2" | "mat3" | "mat4"
                      | "quat" | "bool"

<expression>        ::= <binary_expr>
                      | <unary_expr>
                      | <primary_expr>

<binary_expr>       ::= <expression> <binop> <expression>

<unary_expr>        ::= <unop> <expression>

<primary_expr>      ::= <literal>
                      | <identifier>
                      | <call_expr>
                      | <member_expr>
                      | "(" <expression> ")"
                      | <thread_id_builtin>      ; Phase 3.2 — compute body only

<thread_id_builtin> ::= "thread_id"               ; GLSL gl_GlobalInvocationID (uvec3, Phoskia vec3)
                      | "group_id"                ; GLSL gl_WorkGroupID (uvec3, Phoskia vec3)
                      | "dispatch_id"             ; GLSL gl_NumWorkGroups * gl_WorkGroupID
                      ; 可作为 call form (thread_id()) 或 bare-identifier form
                      ; (thread_id.x)。后者是 Phase 3.2 测试的规范形式：
                      ; let idx = thread_id.x;

<call_expr>         ::= <identifier> "(" <argument_list> ")"

<argument_list>     ::= ""
                      | <expression>
                      | <expression> "," <argument_list>

<literal>           ::= <float_literal>
                      | <int_literal>
                      | <bool_literal>

<float_literal>     ::= <digit>+ "." <digit>*
<int_literal>       ::= <digit>+
<bool_literal>      ::= "true" | "false"

<binop>             ::= "+" | "-" | "*" | "/" | "%"
                      | "==" | "!=" | "<" | "<=" | ">" | ">="
                      | "&&" | "||"

<unop>              ::= "-" | "!" | "not"

<identifier>        ::= <alpha> (<alpha> | <digit>)*
<alpha>             ::= "a"-"z" | "A"-"Z" | "_"
<digit>             ::= "0"-"9"
```

## 11. 实现优先级

### Phase 1: 最小可编译 ✅ 完成

- [x] 词法分析器 (`AYLexer`)
- [x] 语法分析器 (`AYParser`)
- [x] AST 节点定义 (`AYAst`)
- [x] 类型系统骨架 (`AYType`)
- [x] 错误聚合 (`AYCompilerError`)
- [x] BGFX 后端 (`AYBGFXConverter`)
- [x] 编译器流水线 (`AYPhoskia::Compiler`)
- [x] 后端注册机制（多后端接口预留）
- [x] 命名空间分层（`ayt::shader` / `ayt::shader::phoskia`）
- [x] 文件名规范（`AY*.h`/`AY*.cpp`）
- [x] 删除 `AYGenerator`（Phoskia pretty-printer 无意义）
- [x] 删除 `IR` 空壳（留作 Phase 3 TODO）
- [x] 修复 `TokenType::In/Else` 缺失
- [x] 修复 `Parser::error` 空实现
- [x] 删除 `Token::literal` 字段（词法层职责收窄，根治 MSVC SSO 字符串 move 崩溃）
- [x] Lexer 不做数字解析、不做字符串转义（详见 §6.5 设计原则）
- [x] 单元测试（278/278 通过：`AYLexer` / `AYParser` / `AYPhoskia` / `AYBGFXConverter` / `AYShaderCompile`）
- [x] **Phase 1 收尾**: vertex/fragment 双块语法 + in/out Phoskia 语义 + shaderc 三件套（vs/fs/varying.def）端到端

**Phase 1 交付能力**：
- Phoskia 源码 → `.sc` 三件套 → `shaderc.exe` → `.bin` 全链路打通
- 跨 platform 实测：windows / linux / osx / android / ios / asm.js / orbis × 主流 profile（GLSL 1.20、ESSL 3.20、Metal、SPIR-V）
- 失败组合仅限个别 platform/profile（如 osx 不支持 SPIR-V、Windows 不支持 Metal），与 shaderc 自身能力矩阵一致
- `shaderc.exe` 是 bgfx 发布的独立 CLI 工具，**调用方负责 spawn**——`AYBGFXConverter` 只产 `.sc` 文本
- 单元测试默认要求 shaderc 可用，找不到时 fail loudly 并给出修复指引（CMake `-DAY_SHADER_SHADERC_PATH=...` 或环境变量 `AY_SHADER_SHADERC`）

**已知 Phase 1 限制**：
- uniform initializer `uniform vec4 x = vec4(...);` 在 GLSL 1.20 之外的目标 profile（ESSL 3.00、HLSL）不支持——需要 Phase 2 加 `targetProfile` 选项让 converter 按目标 profile 切换
- 真正的多 backend（D3DCompile / glslang / Metal API）属于路线 C（Phase 2/3 范围）
- 每个语义类别在 block 内最多 1 次（texcoord0..7 / color0..1 槽位扩展留 Phase 2）

### Phase 2: 完整 Phoskia 支持
- [x] 类型推导引擎（`TypeInference` 完整实现 — Step 2）
- [x] 内置函数库扩充（scalar/vector/纹理 Step 2，PBR Step 3 已完成：fresnelSchlick / fresnelSchlickRoughness / distributionGGX / geometrySchlickGGX / geometrySmith）
- [x] Variant 宏在 BGFX 后端的 #ifdef 展开 (`[variant name]` → `#ifndef BGFX_VARIANT_<NAME_UPPER>` 包裹，默认 opt-in，详见 §6.3)
- [x] 错误恢复与 panic-mode 验证（Step 4：`Parser::synchronize` 跳过到 statement boundary；`parseMaterialDecl` 内层循环也用 synchronize；EOF / 缺失闭合括号 / garbage token 都不再级联）
- [x] 单元测试与 golden-file 验证（Test_GoldenFiles.cpp 6 个 fixture：5 个 material（unlit / pbr_minimal / pbr_with_emission / pbr_with_texture / empty）+ 1 个 compute_minimal (Phase 3.2)。golden baseline 自动生成 + AY_SHADER_REGEN_GOLDEN env 强制重生成 + 失败时 byte 级 diff 上下文）
- [x] **类型名降级重构**（Step 5 完成：13 个 type keyword（Float/Vec2-4/Int/IVec2-4/Mat2-4/Quat/Bool）从 TokenType enum 删除，Lexer 关键字表清空对应 13 行，parsePrimary / parseShaderParam / consumeTypeName 的临时分支全部移除；新增 AYBuiltinTypes.h/.cpp 提供 string_view 查表 `isBuiltinType`；SemanticAnalyzer 在 analyzeUniformDecl 调用 isBuiltinType 校验非 builtin 名字并报 Go 风格错误"line N: 'hello' is not a builtin type (expected: ...)"）
- [x] **Compute shader 后端** — Phase 2.5 closes the parser / AST half (`compute Name { <body> }` is a top-level declaration, parser builds a `ComputeDecl`, `IRGenerator` lowers to `IRComputeDecl`). BGFX `.sc` does support compute via shaderc `--type compute` + `bgfx::createProgram(_csh)`; the Phase 2.5 placeholder "BGFX .sc does not support compute" error in `AYBGFXConverter.cpp:521` is **stale** and will be replaced by the real emission path in Phase 3.2. See §6.6.
- [x] **Shader type 动态输出变量**（`gl_Position` / `gl_FragColor`，已完成 `_shadingOutputVar`）

### Phase 3: IR 与多后端
- [x] **IR 层定义 + AST→IR 降级 + BGFX 后端 retarget**（Phase 3.1）：IR 是 AST 的 1:1 镜像（`include/AYIr.h`），每个 IR 表达式携带 `resolvedType` 在降级时由 IRGenerator 一次性 resolve；backends 读 `expr.resolvedType` 不再跑 TypeInference。BGFX 已 retarget 完毕，golden + shaderc e2e 全部通过。详见 §6.7。
- [x] **Compiler out-param 重构**（SSO NRVO 根因修复）：`Compiler::compile` / `compileToBackend` 改为 out 参数形式，根除 Phase 3.1 暴露的 MSVC SSO / NRVO 损坏（详见 §6.8）。删除死代码 `CompileResult::typeEnv` / `Compiler::errors()` / `hasErrors()` / 便捷自由函数；Phase 3.2+ 可以安全地往 `CompileResult` 加 per-target 字段。
- [x] **Compute 端到端落地**（Phase 3.2 — BGFX `.sc` compute 路径，2026-06-29 完成）：移除 `AYBGFXConverter.cpp:521` 的 placeholder 报错；实现 `convertComputeDecl` emit 真实的 compute `.sc` 源（见 §6.6.1）；补 `storage NAME : structuredbuffer<T>` / `storage NAME : rwstructuredbuffer<T>` 语法（见 §6.6.2）；补 `thread_id` / `group_id` / `dispatch_id` 0-arg 内置（见 §6.6.3）；`shaderc --type compute` e2e 测试 + golden fixture (`compute_minimal`)。总测试 624 → 690（+66）。

  附带的根因修复：`AYBGFXConverter::convertBGFX` 的 return-by-value 触发 MSVC SSO/NRVO 损坏（同一类 bug，详见 §6.8），新增 out-param 重载 + 全测试切换。

- [x] **Phase 3.3 surface-syntax 补完**（2026-06-30 完成）：
  - [x] `[numthreads(X, Y, Z)] compute Foo { ... }` attribute — commit aa14410
  - [x] `uint` builtin 类型 — commit edd37af
  - [x] 严格 `uvec3` 类型 — `thread_id` / `group_id` / `dispatch_id` 全部返回 `uvec3`（不再是 `vec3`）— commit f3c7e47
  - [ ] 自定义 `struct` 类型（`struct Particle { vec3 pos; vec3 vel; }`）— 推迟到 Phase 4+，需要先做 struct 类型系统
  - [x] `groupshared` 共享存储 — `shared <T> <name>[<size>];` 走 GLSL `shared` 路径 — commit c265ea5

- [x] **Phase 3.4 UBO 落地**（2026-06-30 完成）：`uniformblock` 表面语法 + `layout(std140, binding = N) uniform Name { ... } Name;` emit + 全平台 `-p 430` profile bump + `BGFXUniformBlock` binding info 结构。详见 §6.7。总测试 771 → 786+。

  - 已知 limitation：UBO 字段 strict type-check 暂不在 Phoskia 端做（`let p = Camera.position` 推断为 TypeVar，emit 透明，shaderc 端做类型检查）。完整 struct 推断留 Phase 4+ 跟 struct 类型系统一起做。
  - 推迟：嵌套 UBO / 用户显式 `binding = N` / `storage` decl 的 binding 语法 / HLSL cbuffer packoffset / WGSL `@group @binding var<uniform>`。

- [x] **Phase 3.5-A Storage binding 落地**（2026-07-01 完成）：`storage X : rwstructuredbuffer<T> binding N;` 可选显式 binding 后缀 + `layout(std430, binding = N) buffer X { T data[]; } X;` emit + 编译期 duplicate binding 校验 + auto-binding 起始点 `max(显式)+1` 防撞 + `BGFXStorageBuffer` binding info 结构 + 新关键字 `Binding`。详见 §6.7.7（待添加）。总测试 837 → 852+。

  - 向后兼容：老 storage decl（无 `binding`）继续走"自动分配 slot 0/1/2/..."路径（Phase 3.5-A 之前的行为），emit 不变。`golden/compute_minimal.phoskia` 字节级不变。
  - 推迟：UBO 用户显式 `binding = N`（候选 B）/ HLSL `register(t[N])` / WGSL `@group(0) @binding(N)`。

- [x] **Phase 3.5-B UBO binding 落地**（2026-06-30 完成）：`uniformblock X { ... } binding N;` 可选显式 binding 后缀 + `layout(std140, binding = N) uniform X { ... } X;` emit + 编译期 duplicate binding 校验（per-program 作用域）+ auto-binding 起始点 `max(显式)+1` + 复用 Phase 3.5-A 关键字 `Binding`，**无新词法工作** + 移除 IR 端 `nextBinding_` counter（改为 BGFX emit 端解析）。详见 §6.7.8。总测试 897 → 917+。

  - 向后兼容：老 UBO decl（无 `binding`、无尾 `;`）继续走"自动分配 slot 0/1/2/..."路径，emit 不变。`golden/unlit.phoskia` / `pbr_*.phoskia` 字节级不变。
  - 与 Phase 3.5-A 差异：duplicate 检测作用域是 per-program（UBO 在 vs/fs/cs 间共享），而 storage 是 per-compute（嵌在 `IRComputeDecl::declarations` 里）。
  - 推迟：UBO 字段 strict type-check / 嵌套 UBO / HLSL `cbuffer $Element` / WGSL `@group(0) @binding(N) var<uniform>`。

- [ ] IR 设计实现（SSA 形式）
- [ ] HLSL 后端 (`AYHLSLConverter`) — **Phase 5+ 按需**，当前不计划。仅在项目要求 DXC 一手质量或要摆脱 shaderc 时再做。
- [ ] WGSL 后端 (`AYWGLSConverter`) — **Phase 5+ 按需**，当前不计划。仅在项目目标 WebGPU 且要原生 WGSL 时再做（bgfx 当前没有 WebGPU 后端，需要换 runtime 到 wgpu-native / Dawn）。
- [ ] 跨后端优化（dead code、constant folding）— 在 SSA IR 上做
- [ ] **Ray shader 架构**（独立于 compute 的路径）

### Phase 4: ShaderGraph 与工具链
- [ ] 节点图数据结构
- [ ] 节点 → AST 转换
- [ ] 节点序列化
- [ ] 资源打包与缓存

---

## 14. 当前状态速查（移交用）

### 14.1 已完成能力一览

**编译器核心**（Phoskia → IR）：
- Lexer / Parser / AST 完整（30+ 节点类型）
- TypeInference + SemanticAnalyzer 完整（含 uint / uvec3 strict / builtin 标量向量）
- BuiltinFunctions（PBR + math + texture + compute thread-id 系列）
- IRGenerator（AST → IR 1:1 降级，每表达式预解析 `resolvedType`）
- Compiler 完整 out-param 流水线（根除 MSVC SSO/NRVO bug）

**Shader stage 支持**：
- ✅ Vertex / Fragment / Compute
- ❌ Geometry / Tessellation / Mesh / Task / Ray tracing（bgfx 也没设计）

**资源类型（11 种全支持）**：
| 类型 | Phoskia 语法 | emit 形式 | binding 语法 |
|---|---|---|---|
| Uniform | `uniform vec3 x;` | `uniform vec3 x;` | 自动 |
| Property | `property albedo = vec4(...)` | `uniform vec4 albedo = ...;` | 自动 |
| Texture (2D) | `texture2d albedoMap` | `SAMPLER2D(albedoMap, slot);` | 自动 |
| Storage buffer | `storage X : rwstructuredbuffer<int> binding N;` | `layout(std430, binding = N) buffer X { ... } X;` | ✅ Phase 3.5-A |
| Shared (workgroup) | `shared float tile[64];` | `shared float tile[64];` | N/A |
| UBO | `uniformblock Camera { vec3 pos; float fov; }` | `layout(std140, binding = N) uniform Camera { ... } Camera;` | 自动（Phase 3.5-A 仅 storage） |

**Compute 特性**：
- ✅ `[numthreads(X, Y, Z)]` attribute
- ✅ `thread_id / group_id / dispatch_id` 0-arg 内置（uvec3 strict）
- ✅ `storage` buffer（with explicit binding）
- ✅ `shared` workgroup local memory
- ❌ `barrier / memoryBarrier* / groupMemoryBarrier`（**待做**）
- ❌ `atomicAdd / atomicMin / atomicMax / atomicCompSwap` 等（**待做**）
- ❌ `dispatchIndirect / drawIndirect / drawIndexedIndirect`（**待做**）

**Surface syntax 关键能力**：
- ✅ 类型推导 + let-stmt + binary/unary/call/member/index expr
- ✅ Variant attribute `[variant useEmission]` → shaderc `--define`
- ✅ in/out 语义化参数：`in pos : position;` 替代 bgfx POSITION/NORMAL/COLOR0/TEXCOORD0
- ✅ UBO 块名访问：`Camera.position`
- ✅ PBR 函数库（fresnelSchlick / GGX / Smith / Schlick）

**Backend**：
- ✅ BGFX `.sc`（唯一 backend，走 shaderc 跨 8 平台）
- ❌ HLSL backend（Phase 5+ 按需）
- ❌ WGSL backend（Phase 5+ 按需）

**测试**：
- 897 / 897 PASS
- ~12 测试套件覆盖 lexer / parser / IR / type inference / semantic / BGFX converter / shaderc e2e / golden file / PBR math
- 8 个 golden fixture（6 material + 2 compute）

---

### 14.2 已完成 Phase 索引

| Phase | 范围 | 关键 commit | 测试增量 |
|---|---|---|---|
| 1 | Lexer/Parser/AST/BGFX backend/shaderc e2e | Phase 1 多 commit | 0 → 278 |
| 2.1 | Variant attribute | — | +5 |
| 2.2 | TypeInference + SemanticAnalyzer + 内置函数库 | — | +30 |
| 2.3 | PBR 函数库 | — | +10 |
| 2.4 | Parser panic-mode 错误恢复 | — | +5 |
| 2.5 | Token 降级重构 | — | +5 |
| 2 收尾 | Golden + PBR e2e | — | +5 |
| 2.6 | Compute declaration AST + Parser + BGFX stub | — | +5 |
| 3.1 | Phoskia IR (AYIr) + AST→IR + BGFX retarget | — | +30 |
| 3.2-pre | Compiler out-param 重构（SSO NRVO 根因修复） | — | 0 |
| 3.2 | Compute 端到端 + storage buffer + thread-id | — | +66 |
| 3.3 | `[numthreads]` + uint + uvec3 strict + groupshared | aa14410 / edd37af / f3c7e47 / c265ea5 | +147 |
| 3.4 | UBO 表面语法 + std140 binding | 5a49e8c | +50 |
| 3.5-A | Storage binding 语法 + std430 binding | 57e9c63 / 31e2f6c / 7d93814 / 4058f03 | +60 |
| 3.5-B | UBO binding 语法 + std140 binding（与 3.5-A 对称）| 063664f | +20 |
| 总计 | — | — | **278 → 917** |

---

### 14.3 待办清单（按优先级排序）

#### 🔴 Phase 3.6 — 产品化收尾：`.sc` 作为 backend 内部细节（**最高优先**，用户已 sign-off 2026-06-30）

把 `.sc` 字符串从 frontend API 移走；shaderc 调用内化进 `AYBGFXConverter`；frontend 一次 `compileToProgram(src)` 拿到 `.bin` bytes。详见 §8.4。

| Block | 范围 | 估算 |
|---|---|---|
| 3.6-A | design.md §8.4（本文） | 0.25 天 ✅ |
| 3.6-B | `AYShadercDriver` 抽象 + `AYBGFXConverter::compileToBinary()` 内化 shaderc 调用 + `.sc` 字段从公开移到 private + frontend `compileToProgram()` 暴露 binary API | 1.5-2 天 |
| 3.6-C | Test_ShaderCompile plumbing 重设计（in-memory `.sc` → driver → bin，少写盘层） | 1 天 |
| 3.6-D | `AY_PHOSKIA_DUMP_SC=1`（落盘）+ `AY_PHOSKIA_KEEP_SOURCES=1`（in-memory）双 env 解析与 opts 互转 + unit tests | 0.25 天 |
| **总计** | — | **3-3.5 天** |

#### 🟢 Phase 3.7 — Compute + Texture 补完（**第二优先**，覆盖 90% 真实项目需求）

| # | 能力 | 表面语法 | emit | 估算 | 依赖 |
|---|---|---|---|---|---|
| 1 | **Storage image** | `storageimage X : rwimage2d<float> binding N;` | `layout(r32f, binding = N) uniform image2D X;` + `imageLoad / imageStore` 内置 | 1.5 天 | — |
| 2 | **多 texture kind** | `texturecube envMap;` / `texture3d noise;` / `texture2darray lookup;` | `SAMPLERCUBE / SAMPLER3D / SAMPLER2DARRAY` 宏 | 2 天 | — |
| 3 | **Compute barrier** | `barrier();` `memoryBarrierShared();` 内置 | `barrier();` `memoryBarrierShared();` | 0.5 天 | — |
| 4 | **原子操作** | `atomicAdd(ptr, val);` `atomicMin(...);` 等内置 | `atomicAdd(ptr.data[idx], val);` 等 | 1 天 | — |
| 5 | **Fragment 导数** | `dFdx(v) / dFdy(v) / fwidth(v)` 内置 | `dFdx / dFdy / fwidth` | 0.25 天 | — |

合计：~5 天。**打开 PBR 后处理 / 流体模拟 / GPGPU 通用计算 / 法线贴图等关键场景**。

#### 🟡 Phase 3.8 — 高级渲染特性（第三优先，覆盖剩余 10%）

| # | 能力 | 表面语法 | emit | 估算 | 依赖 |
|---|---|---|---|---|---|
| 6 | **MRT（多 render target）** | `out color1 : color = vec4(0.0);` 加序号 / 命名 | `gl_FragData[0..7] = ...;` | 1.5 天 | — |
| 7 | **Vertex 多 attribute** | `in tan : tangent;` / `in uv2 : texcoord1;` | 扩 `semanticTable()` | 1 天 | — |
| 8 | **Indirect dispatch/draw** | `dispatchIndirect(buf, off, x, y, z);` builtin | bgfx `dispatchIndirect` 宏 | 1 天 | 候选 1 |
| 9 | **Integer sampler** | `texture2di lookup;` / `texture2du lookup;` | `ISAMPLER2D / USAMPLER2D` | 0.5 天 | 候选 2 |
| 10 | **Shadow sampler** | `texture2dshadow shadowMap;` | `SAMPLER2DSHADOW` + PCF 内置 | 0.5 天 | 候选 2 |

合计：~4.5 天。**打开 deferred shading / G-buffer / 高级 PBR / GPU-driven culling 等场景**。

#### 🟠 Phase 3.8 — 工程质量（第四优先，跨阶段）

| # | 能力 | 估算 | 依赖 |
|---|---|---|---|
| ~~11~~ | ~~**UBO 用户显式 binding**（`uniformblock Camera { ... } binding 0;`）~~ —— **Phase 3.5-B 已做**（commit 063664f） | — | — |
| 12 | **UBO field strict type-check**（注册 block 为 StructType，inferMemberExpr 支持 struct） | 1.5 天 | 候选 14 |
| 13 | **SSA IR + 跨后端优化**（constant folding / DCE） | 7-10 天 | — |
| 14 | **struct 类型系统**（`struct Light { vec3 dir; vec3 color; }`） | 3-4 天 | 候选 12 |

合计：~12-15 天。**让 Phoskia 达到生产级 shader DSL 水平**。

#### 🔵 Phase 5+ — 跨后端（按需启动，仅在项目要求时）

| # | 能力 | 触发条件 | 估算 |
|---|---|---|---|
| 15 | HLSL backend（`AYHLSLConverter`，emit HLSL 6.x） | 项目要求 DXC 一手质量 / 摆脱 shaderc | 5-7 天 |
| 16 | WGSL backend（`AYWGLSConverter`，emit WGSL） | WebGPU 目标 + 切 runtime 到 wgpu-native / Dawn | 7-10 天 |
| 17 | Ray tracing shader | DXR / Vulkan RT 需求（需新 backend） | TBD |

#### ⚫ 永不做（明确不做）

- ❌ Geometry / Tessellation / Mesh / Task shader（bgfx 不支持）
- ❌ Bindless texture（Vulkan 风格，bgfx 不支持）
- ❌ Subpass input（Vulkan tile-based 优化，bgfx 不支持）

---

### 14.4 接手人速查表

**首次接手必读**：
1. `README.md` — 用户视角概览 + 当前 status 表
2. `design.md` §1-§5 — 架构总览 + 编译流程 + 核心架构
3. `design.md` §6 — 各模块详细设计（**§6.7 是 IR + 后端核心**，必读）
4. `design.md` §14 — 本节

**改语法的标准流程**（设计在 `design.md` §7）：
1. 改 `design.md` §10 BNF
2. 改 `AYToken.h` enum（加关键字）
3. 改 `AYLexer.cpp` 关键字表
4. 改 `AYAst.h`（加节点 + AstVisitor::visit 重载）
5. 改 `AYParser.cpp`（parseXxx + dispatcher）
6. 改 `AYIr.h` / `AYIr.cpp`（IR 镜像 + lowerDecl）
7. 改 `AYBGFXConverter.cpp`（emit）
8. 加测试：parser / IR / e2e Phoskia / golden fixture
9. 跑 `cmake --build` + `AYShader_Test.exe` 验证
10. 4 块 commit

**关键工程经验**（详见 §6.8 + §15）：
- **MSVC SSO/NRVO bug**：所有返回大 struct 的函数必须 out-param 形式
- **AYTest 全局静态注册**：每个 TEST_CASE 是新 global symbol；CMakeLists 用 `file(GLOB ... CONFIGURE_DEPENDS)`（**两条都改了**：主 lib `CMakeLists.txt` + unittest）
- **黄金 baseline 重生成**：`AY_SHADER_REGEN_GOLDEN=1 ./AYShader_Test.exe`（写到 build-dir，**记得拷回源 dir**）

---

## 15. 工程经验教训（移交用）

### 15.1 MSVC SSO/NRVO 根因（必读）

**症状**：返回 `std::vector<Token>` / `CompileResult` / `BGFXConvertResult` 这类含 SSO std::string 的 struct by value，调用方拿到的字段是 garbage。

**根因**：MSVC 优化器在某些决策下把返回值的 SSO buffer 写到 caller stack 的临时位置，然后该位置被覆盖。

**对策**：
- 所有"返回大 struct"的 API 改 out-param：`void foo(..., Result& out);`
- 已修：`Compiler::compile / compileToBackend`（§6.8）、`AYBGFXConverter::convertBGFX`（§3.2-pre）
- 新 API 永远用 out-param 形式

### 15.2 AYTest 全局静态注册 + CMake 增量 linker bug

**症状**：在已有 .cpp 加新 `TEST_CASE`，跑测试时新测试不出现，必须清理构建目录。

**根因**：
- AYTest 每个 TEST_CASE 是全局静态 `_reg_##name = (registerTest(...), 0);`
- 增量 linker heuristic 可能认为 .exe 已经"fresh enough"不重 link（即使 .cpp mtime 变了）
- 手维护 TEST_FILES list 时 CMake 不知道目录里有变化

**对策**：
- 主 lib `CMakeLists.txt` + unittest `CMakeLists.txt` 都用 `file(GLOB ... CONFIGURE_DEPENDS)`
- `CONFIGURE_DEPENDS` 强制每次构建时 re-glob 目录
- 已修（commits beb2805 + Phase 3.5-A Block 3）

### 15.3 Golden baseline 维护流程

**新增 fixture**：
1. 写 `<name>.phoskia` 到 `unittest/golden/`
2. 在 `Test_GoldenFiles.cpp` 加 `TEST_CASE(golden_<name>) { runOneFixture("<name>"); }`
3. 跑测试（首次会 auto-generate `<name>.sc` 到 build-dir）
4. **手动把 build-dir 的 `<name>.sc` 拷回源 dir**（auto-gen 只写到 build-dir）
5. 跑测试确认 byte-equal

**重生成 baseline**（converter 改了之后）：
1. `AY_SHADER_REGEN_GOLDEN=1 ./AYShader_Test.exe`
2. diff 源 dir 的 .sc 看变化合理
3. build-dir 的 .sc 已经写好（auto-gen 模式会覆盖）
4. 拷回源 dir
5. 跑测试确认 byte-equal
6. 提交新 .sc

### 15.4 shaderc e2e 测试

**前置条件**：
- bgfx vendored under `thirdParty/bgfx-install/{debug,release}/bin/shaderc.exe`
- bgfx source tree at `<sibling>/thirdparty/bgfx/` (for `common.sh` / `bgfx_shader.sh` / `bgfx_compute.sh` includes)

**CMake 变量**：
- `AY_SHADER_SHADERC_PATH` — shaderc 绝对路径（默认 `thirdParty/bgfx-install/debug/bin/shaderc.exe`）
- `AY_SHADER_BGFX_COMMON_DIR` — bgfx `examples/common` 路径（自动搜索）
- `AY_SHADER_BGFX_SRC_DIR` — bgfx `src` 路径（默认从 common 推）

**profile**：当前全部 `-p 430`（UBO `binding = N` 要求 GLSL 4.30+）

---

### Phase 2 重构项：类型名降级为 Identifier（与 type checker 共同推进）

**触发**：Phase 1 调试 `material_declaration_with_property` 时空转，根因是 Lexer 把 `vec3 / float / mat4` 等类型名切成 `TokenType::Vec3 / Float / Mat4` 关键字，而 `parsePrimary()` 没匹配这些关键字，导致 `vec3(1.0, 0.0, 0.0)` 这种类型构造函数返回 nullptr、上层包成 `ExprStmt(nullptr)`、Parser 死循环。

**临时方案（Phase 1，已实施）**：在 `parsePrimary()` 末尾追加 13 个分支（`Vec2 / Vec3 / ... / Bool` → `IdentifierExpr`），让表达式上下文能识别类型名作为构造函数。这是 workaround，每加一个新类型名要同步改 Lexer + Parser 两处。

**目标方案（Phase 2）**——按 Go / Swift / Rust 的工业做法：

1. **`AYToken.h::TokenType` enum** 删除 `Float / Vec2 / Vec3 / Vec4 / Int / IVec2 / IVec3 / IVec4 / Mat2 / Mat3 / Mat4 / Quat / Bool` 共 13 个类型 token。它们**不是**词法概念——字母开头的标识符就是 `Identifier`。
2. **`AYLexer.cpp` 关键字表**对应删除 13 行。
3. **`unittest/Test_Lexer.cpp`** line 36 + line 81–93 改写：类型名断言改成 `tokens[i].type == Identifier && tokens[i].lexeme == "vec3"` 之类。
4. **新增 `AYBuiltinTypes.h/.cpp`**：维护一张 `static const std::unordered_set<std::string_view> builtinTypes = {"float", "vec2", "vec3", ..., "bool"}`，按 `design.md §6.5.10` 用 `string_view` 查表零分配。提供 `bool isBuiltinType(std::string_view)` 接口。
5. **`AYSemanticAnalyzer::analyzeUniformDecl` / `analyzePropertyDecl`** 等"期望类型"的入口，先 match `Identifier` 拿 lexeme，然后调用 `AYBuiltinTypes::isBuiltinType(lexeme)` 判定；不是则报清晰错误"line N: 'hello' is not a builtin type (expected: float, vec2, vec3, ...)"。
6. **`parsePrimary`** 移除方案 C 加的 13 个临时分支，回到只有 `Identifier / FloatLiteral / StringLiteral / True / False / LeftParen` 的干净状态。
7. **`tokenTypeName` debug 函数** 移除对应 13 个 case。

**为什么 Phase 2 才做、不能现在做**：

- `AYTypeInference` / `AYSemanticAnalyzer` 当前是骨架——`analyzeUniformDecl` 只查 `decl.type` 非空字符串，没有 builtin type 校验表
- 现在做方案 A 会导致 `uniform hello x;` 一路通过到 BGFX 后端才崩，错误信息比"compile time syntax error"差得多
- type checker 完工后做方案 A，`uniform hello x;` 在 Semantic 阶段被 `isBuiltinType` 拦住，错误信息质量跟 Go 一样好

**收益**：词法层最简（与 Rust/Go/Swift 对齐），加新类型只改 `AYBuiltinTypes` 一张表。

**风险**：方案 A 完成后，`parseUniformDecl` 等"类型上下文"必须改用 `consume(Identifier, ...)` + `isBuiltinType(lexeme)` 二段式校验，**不能再用 `consume(Vec3, ...)`**——这是行为变化，需要 unnitest 覆盖"非内置类型被拒"。

**前置条件**：`AYTypeInference` 完整实现（`isBuiltinType` 校验表要先于 Lexer 降级落地）。

## 12. 参考

- [bgfx shader system](https://github.com/bkaradzic/bgfx/tree/master/examples/common/shader)
- [WGSL (WebGPU Shader Language)](https://www.w3.org/TR/WGSL/)
- [RSL (RenderMan Shader Language)](https://renderman.pixar.com/resources/RenderMan_20/shadingLanguage.html)
- Unity ShaderLab
- [GLSL ES 3.00 Specification](https://www.khronos.org/registry/OpenGL/specs/gl/GLSLangSpec.3.00.pdf)
