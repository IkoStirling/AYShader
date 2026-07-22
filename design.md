# AYShader Design

> **命名来源**：Phoskia — φῶς (光) + σκιά (影)，光与影的交织，shader 的本质。

## 0. API Stability Promise

> **承诺范围**：`include/AYShader.h` / `include/AYShaderProgram.h` / `include/AYPhoskia.h` / `include/IAYBackendConverter.h` 这 4 个公开头文件的 C++ 符号。
> **承诺生效日**：Phase 4 封顶后（即 §8.5 全部 Block 4-A..4-M 完成；2026-07-01 sign-off 起算）。
> **承诺延展**：Phase 5/6/7 仅扩展 `CompileOptions` 字段，不重排已有字段，不删字段；Phase 8+ 后端替换零 frontend 改动（详见 §14.5）。

### 0.1 ABI 兼容性保证

| 规则 | 适用 | 例外 |
|---|---|---|
| 不删已有 public 字段 | 永远 | 标 `[[deprecated]]` 后保 ≥ 1 个 minor version 才删 |
| 不重排 struct 字段顺序 | 永远 | 同上 |
| 不改字段类型 | 永远 | 同上；扩字段用嵌套 struct + opaque handle |
| 不改函数签名（参数类型 / 返回类型）| 永远 | 加 overload 是允许的（additive）|
| 不改 enum 已有值 | 永远 | 新增 enum 值允许；删除/重命名不允许 |
| 不改 `constexpr` 常量值 | 永远 | 新增 `constexpr` 允许 |

### 0.2 API 演进策略

| 动作 | 标记 | 删除窗口 |
|---|---|---|
| 标 `[[deprecated("reason → new API")]]` | minor version N | minor version N+2 起可删 |
| 加 overload | additive | 无 |
| 加新字段（默认构造）| additive | 无 |
| 加新 enum 值 | additive | 无 |
| 加新 namespace `ayt::shader::v2::*` | additive | 无 |

### 0.3 测试守门

- `Test_HeaderStability`：include 所有公开头，验证 `<bgfx/bgfx.h>` 不出现在 `#include` 树里（grep `clang -M` 输出）
- `Test_ABI_SizeOf`：sizeof(`ShaderResource`) / sizeof(`CompiledShaderProgram`) / sizeof(`CompileOptions`) 在每次 release 锁定值
- `Test_DeprecationWarnings`：deprecated 字段编译期应出 `[[deprecated]]` warning，测试用 `Werror` 检验

### 0.4 升级路径

每次 minor version bump，CHANGELOG 必须列：
- 新增字段 / 函数 / enum 值
- 标 deprecated 的字段 / 函数
- 删除的字段 / 函数（如果有；major version 才允许）

**这与 Rust / TypeScript / Clang 的 API stability policy 同构**（借鉴）。

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

> **Phase 3.6 末现状 / Phase 4 目标**：当前 `AYShaderProgram.h` 仍持有 Phase 1 形态的 `class ShaderProgram`（内含 `bgfx::ShaderHandle`）。frontend 调用方目前**没**用这把（`ShaderCache` 还在草稿阶段），所以泄漏暂时不可见。**Phase 4 起就要把 bgfx 类型从这个头文件里全部挤出去**（pimpl 隔离）。详见 §8.5。

```
┌────────────────────────────────────────────────────────────┐
│  ayt::shader                                               │
│                                                            │
│  IAYBackendConverter.h    — 后端接口（抽象）               │
│  AYBGFXConverter.h/.cpp   — BGFX 后端（Phoskia → .sc）     │
│  AYShadercDriver.h/.cpp   — shaderc driver（Phase 3.6）    │
│  AYShaderProgram.h        — CompiledShaderProgram（bytes） │
│                             + ShaderResource (Phase 4, opaque handle)│
│                             + ShaderResourcePool (Phase 4) │
│  AYShaderCache.h/.cpp     — 编译缓存（Phase 4 收编入 pool）│
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
| `AY_PHOSKIA_DUMP_SC=1` 控制 .sc 落盘 | 与具体 frontend (AYRenderer) 集成 — 见 [`AYRenderer/design.md`](../AYRenderer/design.md) §2 |
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

**Phase 3.6 收尾**：6 个 commit 全部完成（`6c332b0 → c2d128b → 465eec6 → 29db73c → a96b521 → 44e6158`，944/944 测试）。见 §14.2.

#### 8.4.1 CompileOptions Builder Pattern（Phase 6+ 长期目标）

> **借鉴**：[bgfx::ShaderBuilder](https://github.com/bkaradzic/bgfx/blob/master/include/bgfx/bgfx.h) + [Slang Compiler builder](https://github.com/shader-slang/slang/blob/master/docs/user-guide/) + [Bazel rule attribute builder](https://bazel.build/rules/lib/builtins/actions.html)

**当前形态**（Phase 3.6 末）：

```cpp
struct CompileOptions {
    std::vector<std::string> defines;
    bool keepSources = false;
    bool dumpIntermediate = false;
    std::string dumpDir;
};

// 调用方：
CompileOptions opts;
opts.defines.push_back("USE_SHADOWS=1");
opts.keepSources = true;
opts.dumpIntermediate = true;
opts.dumpDir = "./debug";
auto prog = phoskia.compileToProgram(src, opts);
```

问题：
- 字段顺序对调用方要求严格（如果 future 加字段 default 改变，调用方默认行为会变）
- 调用方写 4 行才能完整表达意图
- "一个字段含义多个"，future 加字段容易破坏兼容性

**Phase 6+ 目标**：

```cpp
// 保留 CompileOptions struct 作为 ABI 兼容
// 同时提供 builder helper：
struct CompileOptions {
    std::vector<std::string> defines;
    bool keepSources = false;
    bool dumpIntermediate = false;
    std::string dumpDir;

    // Builder API（Phase 6+ 新增）
    struct Builder;
    static Builder builder();

    // Direct construction still works（ABI compat）
    CompileOptions() = default;
};

struct CompileOptions::Builder {
    Builder& withDefine(std::string_view key, std::string_view value = "");
    Builder& withDefines(std::initializer_list<std::string_view> defines);
    Builder& keepSources();
    Builder& dumpTo(std::string dir);
    Builder& dumpToTempdir();
    CompileOptions build() const;
};

// 调用方（preferred）：
auto prog = phoskia.compileToProgram(src,
    CompileOptions::builder()
        .withDefine("USE_SHADOWS", "1")
        .keepSources()
        .dumpToTempdir()
        .build());

// 调用方（旧 ABI，仍支持）：
CompileOptions opts;
opts.defines.push_back("USE_SHADOWS=1");
opts.keepSources = true;
auto prog = phoskia.compileToProgram(src, opts);  // OK
```

**优势**：
- 默认值隐藏在 builder，**前向兼容**：加新 option 不破坏旧 call sites（builder 默认值自动生效）
- 字段名 = 方法名，IDE 自动补全更友好
- fluent API 可读性高

**§14.6.5 文档同步**：
- 文档同步更新：所有示例从 struct 写法优先迁到 builder 写法（struct 仍允许，标 "advanced"）

**风险与缓解**：

| 风险 | 缓解 |
|---|---|
| Builder 与 struct 双 API 维护成本 | Builder 内部直接构造 struct，无重复 |
| 调用方混淆 | README / example 优先推 builder；struct 写法仍支持 |
| ABI 破坏（struct 字段顺序 / 大小） | Builder 仅加方法，不动 struct 字段；按 §0 API stability 锁定 |

**Phase 6+ 实施步骤**：
- 6-A：加 `CompileOptions::Builder` + 测试
- 6-B：README + examples 切换示例
- 7-A：标 `CompileOptions` 字段 `[[deprecated]]`（"prefer builder.withDefine()")
- 8-A：删 deprecated 字段（major version bump）

---

**问题（暴露给本节）**：当前 `CompiledShaderProgram` 只产出 `.bin` bytes + 元数据。frontend（`AYRenderer` / 任何 host）拿到二进制后**还要自己**再走一遍 bgfx 调用（`createShader → createProgram → createUniform`）才能得到一个可绑定到 draw call 的 program。**这就把 bgfx API 渗到 frontend 了**——`ShaderProgram` 类直接持有 `bgfx::ShaderHandle` / `bgfx::ProgramHandle` 字段（setter/getter 全暴露），frontend 每用一次就要包含 `<bgfx/bgfx.h>` 并面对 bgfx 类型。

```
        现状（Phase 3.6 末）
Phoskia ──compileToProgram──> .bin bytes
                                   │
        ┌──────────────────────────┘
        ▼
AYRenderer (frontend)：byte[] → bgfx::createShader → bgfx::ShaderHandle
                                  → bgfx::createProgram → bgfx::ProgramHandle
                                  → bgfx::createUniform → UniformHandle
                                  ▼
                          把 handle 给 draw call
        (frontend 必须包含 <bgfx/bgfx.h>；耦合泄露)
```

**Phase 4 目标**（本节新设计）：把上述 4 步 bgfx 调用**内化进 AYShader**，frontend 拿到一个 `ShaderResource`（opaque handle），不需要知道背后是 bgfx。整个前端变成：

```
Phoskia ──compileToShaderResource──> ShaderResource (opaque, 引擎无关)
                                          │
                                          ▼
                                 AYRenderer::draw(material, ShaderResource)
                                 bgfx 完全藏在 AYShader 内部
```

详见 §8.5。

### 8.5 Opaque ShaderResource API（Phase 4 设计）

**核心动机**：把 bgfx 调用链从 frontend 收回 AYShader 内部。frontend 一次 `compileToShaderResource(src)` 拿到 opaque handle，自此只跟引擎无关类型打交道。

#### 8.5.1 类型分层

```cpp
namespace ayt::shader {

// 前端（Phoskia）发出的是"无形"产物的三种形态，frontend 按需选一种：
//
//   1) CompiledShaderProgram   — 纯字节（已实现，Phase 3.6）
//   2) ShaderResource          — 不透明 handle（Phase 4 主交付）
//   3) ShaderProgram (legacy)  — bgfx-handle 直接暴露类（Phase 1 老接口，Phase 4 后退役）
//
// 引擎集成：必须用 ShaderResource。byte 形态只用于 cache IO / debugging。
// 老 `ShaderProgram` 类保留作为内部实现细节，不进 frontend API。

class ShaderResource {
public:
    ShaderResource() = default;          // 空 handle；isValid() == false
    bool isValid() const noexcept;

    // === 绑定 ===
    //
    // 在 Phoskia 源码中声明的 uniform / texture / UBO / storage，frontend
    // 通过名字查询 handle。handle 是 opaque ID（实现上 bgfx::UniformHandle
    // 或 AYShaderCache 内部 index），frontend 拿到的值不能解读。
    //
    BindingId getUniformBinding(const std::string& name) const;       // 失败返回 InvalidBinding
    BindingId getTextureBinding(const std::string& name) const;
    BindingId getUniformBlockBinding(const std::string& name) const;
    BindingId getStorageBufferBinding(const std::string& name) const;

    // === 帧期操作（每 draw call）===
    //
    // 抽象掉 bgfx::setUniform / setTexture / submit。frontend 一次给值，
    // AYShader 决定怎么排版（bgfx 平台下走 bgfx::setUniform；future WGSL
    // backend 走 wgpu queue.writeBuffer + setBindGroup）。
    void setUniform(BindingId id, const void* data, size_t sizeBytes) const;
    void setTexture(uint8_t stage, BindingId id, const TextureHandle& tex) const;

    // === 提交（与 AYRenderer draw call 配套）===
    //
    // 一次性提交本帧所有 uniform / texture 设置；AYShader 把它们攒成
    // bgfx batch（DX11 下一次性 update，零额外 draw call cost）。
    void submit(const DrawCallContext& ctx) const;

private:
    friend class ShaderResourcePool;    // 创建受 pool 管理，不允许外部 new
    std::shared_ptr<ShaderResourceImpl> _impl;   // pimpl，pimpl 内部含 bgfx handles
};

using BindingId = uint32_t;             // opaque numeric ID
constexpr BindingId InvalidBinding = 0; // 0 = 不存在 / 无效

// 用法：frontend 不直接持有 bgfx handle；通过 AYRenderer::submit 时把
// ShaderResource 当作一个 handle 传给 draw call 即可。
} // namespace ayt::shader
```

#### 8.5.2 Compilation entry point

```cpp
namespace ayt::shader::phoskia {

class Compiler {
public:
    // 旧 (Phase 3.6) — 保留；用于 cache IO / 跨后端 binary diff
    CompiledShaderProgram compileToProgram(const std::string& src);
    CompiledShaderProgram compileToProgram(const std::string& src,
                                            const CompileOptions& opts);

    // 新 (Phase 4) — frontend 主路径
    // CompileToShaderResource 把 4 步 bgfx 调用内化进 AYShader：compile →
    // shaderc → bgfx::createShader → bgfx::createProgram → 缓存 handle
    // pool → 返回 opaque ShaderResource。
    //
    // 与 compileToProgram 的区别：
    //   - 必须有活跃的 ShaderResourcePool（详见 §8.5.4）
    //   - 失败时返回 ShaderResource{} 空 handle（isValid()==false）
    //   - 自动 hot-reload 失效缓存（详见 §9）
    ShaderResource compileToShaderResource(const std::string& src);
    ShaderResource compileToShaderResource(const std::string& src,
                                            const CompileOptions& opts);
};
}
```

#### 8.5.3 Hide 实际 contract

**frontend 应该永远见不到**：

| 类型 | 原因 |
|---|---|
| `bgfx::ShaderHandle` | bgfx 内部 OPAQUE；frontend 必须不感知 |
| `bgfx::ProgramHandle` | 同上 |
| `bgfx::UniformHandle` | 同上；frontend 通过 `BindingId` 间接操作 |
| `bgfx::TextureHandle` | frontend 自己 graphic resource 仍持有，但**通过 `ShaderResource::setTexture` 喂给 shader 不直接调 bgfx** |
| `BGFXShaderFiles::vs/fs/varyingDef` | Phase 1 老字段，Phase 4-H 顺手删除 |
| `BGFXConvertResult` | backend 内部形态 |

**frontend 直接见到的**：

| 类型 | 说明 |
|---|---|
| `ShaderResource` | opaque handle（pimpl） |
| `BindingId` | uint32_t，opaque |
| `TextureHandle` | 由 AYRenderer 提供，**driver 中立**（bgfx backend / 未来 wgpu backend 都用同一类型） |
| `CompiledShaderProgram` | 调试 / 跨进程传输用 |
| `CompileOptions` | 已有 |
| `CompileResult` | 已 deprecated（保留向后兼容） |

#### 8.5.4 Pool 拥有权 + 生命期

```cpp
namespace ayt::shader {

// Pool 是 ShaderResource 的"工厂 + 所有者"。Phase 4 主交付物之一；
// engine 启动时构造一个 pool，destroy 时所有 ShaderResource 句柄随之失效。
//
// 设计动机：
//   - ShaderResource 内部含 bgfx handles（pimpl → `bgfx::ShaderHandle`），
//     bgfx::shutdown() 之后这些 handle 就是悬空的。让 pool 在 shutdown
//     之前 release 所有 handle，frontend 没法误用。
//   - pool 也管 cache（同 source 二次 compile 走 byte-cache hit），见 §9
//   - pool 也管 hot-reload（source 改了 → 旧 ShaderResource 自动 invalidate）
class ShaderResourcePool {
public:
    ShaderResourcePool();
    ~ShaderResourcePool();

    // 主调用：compile + link + cache + return
    ShaderResource acquire(const std::string& src,
                           const CompileOptions& opts = {},
                           const std::string& cacheKey = "");

    // 显式释放（pool 析构时也会自动）
    void release(ShaderResource& res);

    // 全局资源回收（bgfx::shutdown 之前 engine 调一次；保证安全退出）
    void shutdown();

    // Hot-reload（开发模式）：pool 检测到 source 改了，自动让缓存中的
    // ShaderResource 失效。frontend 调用 path 不需要变 — 反正它只是用
    // opaque handle。开发期 vs release 期行为不同 — release 期这个 hook
    // 是 no-op。
    void setHotReloadEnabled(bool enabled);

private:
    std::unique_ptr<ShaderResourcePoolImpl> _impl;
};

}
```

**frontend 启动 pattern**：

```cpp
// Engine startup (AYEngine::init 或类似处)
ayt::shader::ShaderResourcePool shaderPool;
shaderPool.setHotReloadEnabled(EngineDebugMode());
// 在 Engine 关闭前调一次（保证在 bgfx::shutdown() 之前）
shaderPool.shutdown();
```

##### 8.5.4.1 Opaque handle ABI 收紧（Phase 4-O）

> **借鉴**：[bgfx::ShaderHandle (uint16_t)](https://github.com/bkaradzic/bgfx/blob/master/include/bgfx/bgfx.h#L52) + [Vulkan VkPipeline (opaque handle)](https://www.khronos.org/registry/vulkan/specs/1.3-extensions/man/html/VkPipeline.html) + [Microsoft COM IUnknown*](https://learn.microsoft.com/en-us/windows/win32/com/com-object-interfaces)

**当前 §8.5 状态（Phase 4-A 初版）**：

```cpp
class ShaderResource {
    // pimpl：handle 在 Impl 内部
    std::shared_ptr<const ShaderResourceImpl> _impl;
    // frontend 仍然能看见一些 metadata 字段（name / variants / bindings）
    std::string _name;
    std::vector<Variant> _variants;
    std::unordered_map<std::string, BindingId> _bindings;
};
```

问题：
- frontend 可以存整个 `ShaderResource` 对象（"res._name" / "res._bindings"），但 ABI 与具体 backend 强相关
- pool LRU 释放某个 shader 时 frontend 持有的 `ShaderResource` 拷贝里的 `_impl` 还指向旧 Impl

**Phase 4-O 目标**：

```cpp
// include/AYShaderProgram.h
namespace ayt::shader {

// 1) ShaderResource 是纯 opaque handle，**单一公开字段**
class ShaderResource {
public:
    ShaderResource() noexcept;                      // 默认构造 = 空
    explicit ShaderResource(uint64_t id) noexcept;  // pool 内部用
    
    bool isValid() const noexcept;
    void reset() noexcept;
    
    // 比较 / hash：可作为 std::unordered_map key
    bool operator==(const ShaderResource& o) const noexcept;
    bool operator!=(const ShaderResource& o) const noexcept;
    
    // copy/move 都允许（只复制 _id，不复制 backend 状态）
    ShaderResource(const ShaderResource&) = default;
    ShaderResource& operator=(const ShaderResource&) = default;
    
    // 2) frontend 用 handle 调 ops（不是直接拿 metadata）
    BindingId getUniformBinding(std::string_view name) const;
    BindingId getTextureBinding(std::string_view name) const;
    void setUniform(BindingId id, const void* data, size_t bytes);
    void setTexture(BindingId id, TextureHandle h);
    void submit(DrawCallContext& ctx);
    
private:
    uint64_t _id = 0;  // ← **唯一公开字段；pimpl 在 ShaderResourcePool 内部表里**
};

// 2) Pool 是 ShaderResource 的实际所有者（"handle table"）
class ShaderResourcePool {
    std::unordered_map<uint64_t, std::unique_ptr<ShaderResourceImpl>> _table;
    // LRU / explicit release / shutdown 都更新这张表
};

} // namespace
```

**关键 contract**：

```cpp
// frontend 可以随便拷贝、移动 ShaderResource —— 只复制 _id：
ShaderResource a = pool.compile(src);
ShaderResource b = a;   // OK; b._id == a._id
auto map_key = std::unordered_map<ShaderResource, MyData>{};
map_key[a] = data;      // OK; ShaderResource 是 hashable
```

**Pool 内部 LRU 释放（不会让 frontend handle 失效）**：

```cpp
// pool 内部实现：
class ShaderResourcePool::Impl {
    std::unordered_map<uint64_t, std::unique_ptr<ShaderResourceImpl>> _table;
    std::list<uint64_t> _lru_order;
    
    void evictIfNeeded() {
        if (_table.size() > _max_size) {
            uint64_t evictId = _lru_order.front();
            _lru_order.pop_front();
            _table.erase(evictId);  // ← 释放 Impl
            // frontend 持有的 ShaderResource{evictId} 仍合法（_id 不变）
            // 但下次调 .submit() 时 pool 检测到 _id 不在 _table → return null / warn
        }
    }
};
```

**§14.6.1 测试守门**：
- `Test_HandleABI`：sizeof(`ShaderResource`) == 8（uint64_t）；copy / move 后 `_id` 不变
- `Test_LRUEviction`：pool evict 后 frontend 持有的 `ShaderResource{evicted_id}` 仍合法但 `isValid() == false`
- `Test_HandleAsMapKey`：`std::unordered_map<ShaderResource, T>` 编译通过

**风险与缓解**：

| 风险 | 缓解 |
|---|---|
| handle 重用（uint64 用完）| 64-bit 空间够大（2^64 = 1.8e19）；每 evict 不 reuse id（递增）|
| frontend 误用 evict 后的 handle | `setUniform/submit` 在 handle invalid 时 no-op + warn（不崩）|
| `BindingId` 也需要类似收紧 | `BindingId = uint32_t` 已 opaque（§8.5.1）；同 design pattern |

#### 8.5.5 与 binding metadata 的对应

**frontend 不需要从 `CompiledShaderProgram` 读 `BGFXUniform{name, type, count}` 然后**自己**映射到 bgfx**：

```cpp
// 旧 path（Phase 3.6，frontend 手做 bgfx wire）
auto prog = phoskia.compileToProgram(src);
for (const auto& u : prog.uniforms) {
    bgfx::UniformHandle h = bgfx::createUniform(u.name.c_str(),
                                                toBgfxType(u.type),
                                                u.count);
}
// 还要把 type name (string) → bgfx::UniformType::Enum 映射
// 还要把 BGFXTexture → bgfx::TextureHandle 映射
// frontend 必须包含 <bgfx/bgfx.h>，还必须知道 toBgfxType() 在哪里
```

**新 path**：

```cpp
// Phase 4：上述全部移到 AYShader 内部；frontend 只拿 name
ShaderResource res = phoskia.compileToShaderResource(src);
BindingId modelViewProjId = res.getUniformBinding("modelViewProj");
// 给值：直接拿内存字节
res.setUniform(modelViewProjId, &mvpMatrix, sizeof(mvpMatrix));
res.submit(drawCtx);
```

| 名称 | Phoskia 源码 | Phase 4 API |
|---|---|---|
| 普通 uniform | `uniform vec3 lightDir;` | `res.getUniformBinding("lightDir")` |
| Property | `property emission = vec3(0);` | 同上（property 内部就是 uniform） |
| Texture | `texture2d albedoMap;` | `res.getTextureBinding("albedoMap")` |
| UBO | `uniformblock Camera { vec3 pos; }` | `res.getUniformBlockBinding("Camera")` → 一整块 std140 |
| Storage buffer | `storage counters : rwstructuredbuffer<int>` | `res.getStorageBufferBinding("counters")` |
| Builtin uniform（`u_time` 等 system uniform） | — | 预留 `getBuiltinUniform(...)`，Phase 4+ 决定 |

##### 8.5.5.1 Two-tier cache 设计（Phase 4-Q）

> **借鉴**：[Unreal Derived Data Cache (DDC)](https://docs.unrealengine.com/5.0/en-US/derived-data-cache-in-unreal-engine/) + [Unity AssetBundle cache](https://docs.unity3d.com/Manual/AssetBundlesIntro.html) + [bgfx ShaderCache](https://github.com/bkaradzic/bgfx/blob/master/src/shader.cpp)

**当前 §9 / §8.5 状态（Phase 4-I 单层 binary cache）**：

```
compile(src) → cacheKey = SHA256(src+defines+backend) → binary cache
```

问题：
- **source cache 缺失**：frontend 调试时改 `src` 想看 AST 错误，cache 不会失效到 "重新 parse" 这层；如果 cache hit 直接拿 binary，没有 AST 错误信息
- **defines 改了** → cache miss → 重新跑 lexer + parser + backend + shaderc。**3 层重复劳动**
- frontend 想做 incremental compile（"只改了一个 material" → "只重编这一个"）缺支撑

**Phase 4-Q 目标**：

```
┌─────────────────────────────────────────────────────────────────┐
│  Two-tier cache                                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Tier 1: Source Cache                                           │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ Key:    SHA256(phoskia source)                           │  │
│  │ Value:  IRProgram (AST + typed IR + reflection)          │  │
│  │ Invalidation: source 改了 → 重 parse + semantic           │  │
│  │ Hit rate: 极高（同一 source 多次 compile）                │  │
│  └──────────────────────────────────────────────────────────┘  │
│                              ↓                                  │
│  Tier 2: Binary Cache                                           │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ Key:    SHA256(source + defines + capability             │  │
│  │                   + platform + profile + includeDirs)    │  │
│  │ Value:  std::vector<uint8_t>  + bgfx::ShaderHandle       │  │
│  │ Invalidation: defines/capability/platform/profile        │  │
│  │                /includeDirs 任意改了 → 重 backend+shaderc │  │
│  │ Hit rate: 高（跨 defines 变化的部分 binary 可 reuse）     │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**API**：

```cpp
// AYShaderPool 内部：
class ShaderResourcePool::Impl {
    // Tier 1
    std::unordered_map<std::string, std::shared_ptr<phoskia::IRProgram>> _sourceCache;
    
    // Tier 2
    std::unordered_map<std::string, std::shared_ptr<CompiledBinary>> _binaryCache;
    
    // 公开统计（frontend 调试用）
    struct CacheStats {
        size_t sourceHits = 0, sourceMisses = 0;
        size_t binaryHits = 0, binaryMisses = 0;
    };
    CacheStats stats() const;
};
```

**Invalidation 规则**（写进 §14.6.2 design review checklist）：

| 触发 | Tier 1 | Tier 2 |
|---|---|---|
| `src` 改 | miss → 重 parse + semantic | miss → 重 backend + shaderc |
| `defines` 改 | hit（IR 不变）| miss → 重 backend + shaderc |
| `capability` 改 | hit | miss → 重 backend（pool 选新 backend）|
| `platform` / `profile` 改 | hit | miss |
| `includeDirs` 改 | hit | miss |
| Phase 4-O LRU 释放 binary cache | — | evict；frontend handle 仍合法（§8.5.4.1）|

**Persistence（可选 Phase 5+）**：

- Tier 1 / Tier 2 都可走 `setCacheDirectory(...)` 落盘
- 落盘 format：`{hash}.ir.bin` / `{hash}.shader.bin`
- 启动时 lazy-load；命中跳过 compile
- 设计类比 Unreal DDC：source 改 hash → 自动 miss；engine 升级改 binary format → version stamp

**§14.6.2 测试守门**：
- `Test_CacheInvalidation`：改 defines → tier 2 miss、tier 1 hit
- `Test_CacheStats`：跑 N 次相同 src，stats().binaryHits 递增
- `Test_CachePersist`：写 cache 到 disk，重启后能命中（Phase 5+）

**风险与缓解**：

| 风险 | 缓解 |
|---|---|
| 两层 cache 内存占用翻倍 | Tier 1 IR 比 binary 小一个数量级；可调 max_size |
| source cache key 用 SHA256 太重 | IR cache hot path 用 `std::unordered_map`；只有 miss-to-disk 时算 SHA256 |
| 落盘 cache 与 backend version 不匹配 | version stamp 在 cache file header；不匹配全清空 |

---

#### 8.5.6 Phase 4 范围 / 不在范围

| 在 Phase 4 | 不在 Phase 4 |
|---|---|
| `ShaderResource` opaque handle 类 | WGSL/WebGPU backend（Phase 5+） |
| `ShaderResourcePool` 拥有权 + 缓存 | HLSL raw 输出 + DXC 替代（Phase 5+ 候选） |
| `compileToShaderResource(...)` 入口 | 跨 pool 共享（多个 pool 场景） |
| bgfx 4 步调用全部内化 | shader variant management（仍是 `[variant name]` + 自动选择） |
| binding 名 → opaque ID 映射（含 type 推断） | shader pre-warm（提前 compile 不等首次用） |
| `submit(DrawCallContext)` 整合 | GPU pipeline state object 抽象（Phase 5+） |
| hot-reload（dev only） | — |
| Phase 4-H 顺手删 `.sc` 公开字段 | — |

#### 8.5.7 验收 contract

```cpp
// §8.5 acceptance：
//
// 1. frontend 不需要 include <bgfx/bgfx.h>：
//    - AYShader 主头文件 AYShader.h 不出现 bgfx::* 类型
//    - 任何 *.h in include/ 不能 bring in bgfx 符号（pimpl 隔离）
//
// 2. 关键 API 全部 opaque：
//    - BindingId is uint32_t
//    - ShaderResource 是 pimpl
//    - getXxxBinding("name") 返回 opaque ID
//
// 3. compile 链单位一：
//    - phoskia.compileToShaderResource(src) 等价于
//      "byte compilation + bgfx wire + cache"，frontend 零额外调用
//
// 4. 后端可换：Phase 5+ 切换 WGSL backend 时 frontend 代码不变
//    （compileToShaderResource 签名不变；PoolImpl 内部切 wgpu）
//
// 5. 测试：Test_ShaderCacheIntegration 应验证一个真实 frontend 场景
//    （取 ShaderResource → setUniform → 模拟 submit）全程零 bgfx header
//    visible 至 compilation unit。
```

#### 8.5.8 风险与缓解

| 风险 | 缓解 |
|---|---|
| `pimpl` 引入 shared_ptr 开销（每次 ShaderResource 拷贝增加 1 次 atomic refcount） | `ShaderResource` 用 intrusive / SSO 形式（`eastl::intrusive_ptr` 或自制）；Phase 4 后 benchmark |
| bgfx::createUniform 的 type 字符串 → bgfx::UniformType::Enum 映射表在 AYShader 内部维护，frontend 不知道 | 在 AYShader 内部用 lookup 表（`std::unordered_map<std::string, bgfx::UniformType::Enum>`，named bgfx_TypeNameMap）；不外漏 |
| hot-reload 误命中（编辑器保存触发误 invalidate） | dev-only feature（release build 编译成空 hook）+ 文件 mtime 检查 + 100ms debounce |
| pool 顺序析构问题（bgfx 已 shutdown 但 pool 还在） | pool 持有 BGFX 关闭 hook，强制 engine 在 bgfx::shutdown() 之前调 pool->shutdown() |
| ShaderResource 跨线程使用 | bgfx 本身**不是 thread-safe**；Phase 4 锁 PHOSKIA_CALL_ON_RENDER_THREAD 宏，未来 multi-thread render 再讨论 |

#### 8.5.9 与 Phase 3.6 / Phase 4-H 的关系

| 项 | 关系 |
|---|---|
| Phase 3.6 `compileToProgram` | 保留 — cache IO / 调试 / 跨进程传输需要 |
| Phase 4-H 删 `.sc` 字段 | 不冲突 — backend 内部彻底清理 |
| 现有 `ShaderProgram` 类（持有 bgfx::ShaderHandle） | **退役**：Phase 4 后只作 ShaderResourceImpl 内部持有，frontend 永不见 |
| 现有 `AYShaderCache` | 收编进 `ShaderResourcePool::Impl`（cache hit 是 pool 的责任） |
| Phase 4 内部使用 `CompiledShaderProgram` | **可以** — pool->acquire 内部先 compileToProgram 拿 bytes，再 bgfx::createShader，用完丢弃 — 字节不外漏 |

#### 8.5.10 Engine-driven config consolidation（单一入口设计 — 用户 sign-off 2026-07-01）

**核心论证**：fragment / vertex / compute 是**资源形态**（Phoskia 源码内声明）。但**目标 backend + 目标平台 + shaderc 路径**是**引擎配置**，不是资源属性。当前 `BGFXCompileOptions` 把这两类混在一起 — frontend 想 compile 一段 Phoskia 还要主动选 backend（"bgfx"）和平台（"linux" / "windows"）。这暴露了不该 frontend 见的配置。

**当前痛点**（Phase 3.6 末 / § 8.4 状态）：
```cpp
// frontend 现状：compile 时仍要填 backend / platform / include
phoskia::Compiler compiler;
phoskia::CompileOptions opts;
opts.targetBackend = "bgfx";       // ← frontend 选 backend？
opts.includeDirs  = {bgfxCommon, bgfxSrc};  // ← frontend 知道 bgfx source 在哪？
opts.platform = "linux";           // ← frontend 知道目标 platform？
opts.profile = "430";              // ← frontend 选 GLSL profile？
// 上方 4 个全部是 engine-side config，但散落到每个 call
```

##### 8.5.10.1 Capability-based backend selection（Phase 4-L）

借鉴 WebGPU `device.adapter.features` / Vulkan `vkPhysicalDeviceFeatures` / bgfx `bgfx::RendererType`——frontend 应该描述**能力需求**，不指定 backend name。

**目标（Phase 4 核心交付）**：

```cpp
// ===== 引擎 startup：一次性配置（典型是一处） =====
ayt::shader::ShaderResourcePool shaderPool;

// Phase 4-L 核心：frontend 不再选 "bgfx" 这个字符串，而是描述 capability
shaderPool.require(ShaderCapability::COMPUTE     // 需要 compute shader
                 | ShaderCapability::SSBO       // 需要 storage buffer
                 | ShaderCapability::UBO);      // 需要 uniform buffer

// pool 内部从已注册的 backend 选："bgfx on Vulkan" / "wgpu-native" / 等
// frontend 永不见 backend name

shaderPool.setShadercExecutable("/path/to/shaderc.exe");
shaderPool.setBgfxIncludeDirs({"/path/to/common", "/path/to/src"});
shaderPool.setCacheDirectory("~/.cache/ay/shaders/");
shaderPool.setHotReloadEnabled(EngineDebugMode());

// ===== frontend per-shader 调用（典型是 N 处） =====
ayt::shader::ShaderResource r = shaderPool.compile(src);
// frontend 一行即可；不需要知道 backend / platform / capability 的存在
// `src` 是 Phoskia 源码字符串
```

**Capability enum 定义**：

```cpp
enum class ShaderCapability : uint32_t {
    NONE             = 0,
    VERTEX_FRAGMENT  = 1u << 0,   // vs + fs pipeline
    COMPUTE          = 1u << 1,   // cs pipeline
    UBO              = 1u << 2,   // std140 binding
    SSBO             = 1u << 3,   // std430 binding
    STORAGE_IMAGE    = 1u << 4,   // Phase 5+：rwimage
    MULTI_TEXTURE    = 1u << 5,   // Phase 5+：texturecube/3d/array
    ATOMIC           = 1u << 6,   // Phase 5+：atomicAdd/Min/Max
    INDIRECT_DISPATCH= 1u << 7,   // Phase 6+：dispatchIndirect
    INTEGER_SAMPLER  = 1u << 8,   // Phase 6+：texture2di/u
    SHADOW_SAMPLER   = 1u << 9,   // Phase 6+：texture2dshadow
};
inline ShaderCapability operator|(ShaderCapability a, ShaderCapability b) { ... }
```

**Backend 注册**（pool 内部，user 不见）：

```cpp
// ayt::shader::detail::registerBuiltinBackends(pool):
//   pool._registerBackend("bgfx-vulkan",  {capability: VF|UBO|SSBO|COMPUTE|...}, factory);
//   pool._registerBackend("bgfx-d3d11",   {capability: ...}, factory);
//   pool._registerBackend("wgpu-native",  {capability: ...}, factory);  // Phase 8+
//
// 当 pool.require(COMPUTE | SSBO) 被调：
//   - 遍历注册表 → 找 capability 覆盖的 → 按优先级（"render path 优先" / "mobile 优先"）选
//   - 缓存选择结果；之后 compile 用同一个 backend
```

**为什么是 capability-based**：
- frontend 不该关心 "我用 bgfx 还是 wgpu"——这是实现细节
- Phase 8+ 加 WGSL backend 时 frontend 代码零改动（capability 接口不变）
- 与 Unity `GraphicsDeviceType` 抽象 + Vulkan feature query 同构

**§14.6.1 测试守门**：
- `Test_CapabilityDispatch`：同一个 frontend compile call 在 pool require 不同 capability 时走不同 backend path
- frontend TU 不出现字符串 "bgfx" / "wgpu" / "hlsl" / "vulkan"（编译期 grep 验证）

**frontend-side `phoskia::CompileOptions` 重设计**——只保留真正 per-call 的字段：

```cpp
// include/AYPhoskia.h::CompileOptions（Phase 4 后）
struct CompileOptions {
    // Variant definitions（per-shader，唯一真正属于 frontend 的字段）
    // 例：用户对某个材质定义了 [variant useEmission]，
    // engine 想强制开启 → 通过 defines 喂给编译
    std::vector<std::string> defines;

    // Debug toggles
    bool keepSources = false;
    bool dumpIntermediate = false;
    std::string dumpDir;

    // ❌ 全部下移到 ShaderResourcePool——Phase 4 删:
    // - targetBackend（默认由 pool 决定）
    // - includeDirs（默认从 bgfx source tree 自动定位）
    // - platform / profile（默认从 pool / bgfx::getCaps 探测）
    // - enableTypeInference（frontend 真不关心）
    // - enableSemanticAnalysis（同上）
};

// pool 入口只接收 per-call 形式：
class ShaderResourcePool {
    ShaderResource compile(const std::string& src);                  // 默认 opts
    ShaderResource compile(const std::string& src, const CompileOptions& opts);
    ShaderResource compile(const CompileResult& frontedResult);      // 已 parse 复用
};
```

**自动 platform 探测合约**：

```cpp
// ShaderResourcePool 构造时若没显式 setPlatform，
// 调 bgfx::getCaps() 读当前 runtime 的 renderer type：
//
//   bgfx::RendererType::Direct3D11    → platform="windows"   profile="430"
//   bgfx::RendererType::Direct3D12    → platform="windows"   profile="430"
//   bgfx::RendererType::OpenGL        → platform="linux"     profile="430"
//   bgfx::RendererType::Vulkan        → platform="linux"     profile="430"
//   bgfx::RendererType::Metal         → platform="osx"       profile="metal"
//   bgfx::RendererType::WebGPU        → platform="wasm"      profile="wgsl"
//
// 这把 "platform 选择" 的责任完全从 frontend 拿开。
// Engine 启动顺序自然保证：
//   1. bgfx::init(...)
//   2. ShaderResourcePool pool; ← 此时 bgfx caps 已就绪
//   3. pool.setDefaultBackend(...)  / setShadercExecutable(...)
//   4. 之后任意 frontend 调用都不再关心 backend / platform
```

**frontend 完全见不到了**：

| 旧 public field | 旧可见位置 | Phase 4 后位置 | frontend 还见？ |
|---|---|---|---|
| `targetBackend` | `phoskia::CompileOptions` | pool 构造 / `setDefaultBackend` | ❌ |
| `platform` / `profile` | `BGFXCompileOptions` | pool `setPlatform` 或 auto from bgfx caps | ❌ |
| `includeDirs` | `BGFXCompileOptions` | pool `setBgfxIncludeDirs` 或 auto from bgfx source tree | ❌ |
| `shadercPath` | `BGFXCompileOptions` + `AYShadercDriver::setDefaultExecutable` | 收到 pool 内部 | ❌ |
| `enableTypeInference` / `enableSemanticAnalysis` | `phoskia::CompileOptions` | pool 内部 (Phase 4 默认 on；frontend 不该关) | ❌ |
| `defines` | `phoskia::CompileOptions` | **保留** — 这才是真正 per-shader 的字段 | ✅ |
| `keepSources` / `dumpIntermediate` / `dumpDir` | `phoskia::CompileOptions` | **保留** — debug toggle per-shader 合理 | ✅ |

**cache key 设计**：
cache hit 必须依赖全部 backend 配置都相同——否则一份"linux profile=430" 的 cache 给"windows profile=440" 用就错了。

```cpp
struct CacheKey {
    std::string           sourceHash;      // SHA256 of phoskia source
    std::string           definesHash;     // SHA256 of defines[]
    std::string           backendName;     // "bgfx"
    std::string           platform;        // "linux" / "windows" / ...
    std::string           profile;         // "430" / "440" / "metal" / "wgsl"
    std::vector<std::string> includeDirs; // bgfx source dirs (cache 失效时重 hash 一次)
};
// 任意字段变了 → cache miss → 重新 compile
```

**配置层级命名（统一命名空间）**：

| 概念 | 命名 | 例子 |
|---|---|---|
| 引擎的图形 API 选择 | `backend` | `"bgfx"` / `"hlsl"`（Phase 8） |
| 引擎的 OS / runtime 平台 | `platform` | `"linux"` / `"windows"` / `"metal"` |
| 引擎的 shader 兼容 profile | `profile` | `"430"` / `"metal"` / `"wgsl"` |
| include 路径 | `includeDirs` | bgfx common.sh + src |
| compile 调用方差异（variant / debug） | `defines` / `keepSources` / `dumpIntermediate` | per-call |

**phase 4 完整 frontend 单一入口 contract**：

```cpp
// 唯一允许的 frontend 调用：
ShaderResource r = pool.compile(src);
// 唯一允许的 frontend 配置（per-call）：
pool.compile(src, {.defines={"BGFX_VARIANT_USE_EMISSION"},
                   .keepSources=true});
// 唯一不允许的 frontend 行为：知道 backend / platform / include 任何一词
```

**对测试的连锁影响**：

| 测试类型 | 影响 |
|---|---|
| `Test_ShaderCacheIntegration`（Phase 4 新增） | 验证：compile 调用不需要 frontend 关心 backend——整个 TU 不带 bgfx header |
| `Test_Phoskia` / `Test_BGFXConverter` | 当前每个测试都带 `BGFXCompileOptions`；Phase 4-A 后迁到 pool-based，frontend 测试 fixture 用 `ShaderResourcePool` setup |
| `Test_ShaderCompile`（e2e） | 改用 pool；与现有 shaderc 设置复用（pool 内部仍调 `setDefaultExecutable`） |
| 现有 engineless 测试 | 单元测试可保持 direct `BGFXCompileOptions` 路径（pool 是 wrapper，不需要强制替换） |

**风险与缓解**：

| 风险 | 缓解 |
|---|---|
| Pool 配置错误（忘了 setShadercExecutable）→ 第一次 compile 才报错 | `pool.compile()` 立即 fail-fast：构造期不强制，但第一次 compile 检查必需字段齐；返回 `ShaderResource{}` 空 handle（isValid()==false） |
| bgfx::getCaps() 在 pool 构造时还没 init bgfx → 探测失败 | pool 暴露 `lazyProbePlatform()`：首次 compile 时才探；或要求 `pool.bindRendererType(rendererType)` 显式注入（避免隐式顺序依赖） |
| shaderc.exe / bgfx source tree 路径在用户机器变化 | pool startup 配置失败 → 友好错误（带"在哪设置这个 config" 提示） |
| cache key hash 漏算某个字段 → cache false hit | 测试覆盖：改 pool config 后 cache miss（用 ingest rate 检测） |
| Pool 接口破坏性变化（Phase 4 中途） | 设计上 Pool API 一次定下，**之后只追加**——frontend 编写时即按 Phase 4 末形态写 |

**与 Phase 4-K e2e 测试的 contract**：

```cpp
// Test_ShaderCacheIntegration.cpp（Phase 4-K 新增文件）：
TEST_CASE(phase4_no_bgfx_header_in_frontend_tu) {
    // grep / static_assert:
    // - AYShader.h 不含 bgfx::* 类型
    // - AYShaderProgram.h 不含 bgfx::* 类型
    // - 任何调用 ShaderResource API 的 TU 不含 #include <bgfx/bgfx.h>
}

TEST_CASE(phase4_pool_drives_everything) {
    ayt::shader::ShaderResourcePool pool;
    pool.setDefaultBackend("bgfx");
    pool.setShadercExecutable(...);
    pool.setBgfxIncludeDirs({...});
    pool.setPlatform("linux");        // 显式（无 bgfx runtime 时）
    pool.setGLSLProfile("430");

    // 唯一允许 frontend 做的：
    auto r = pool.compile("material X { vertex { ... } fragment { ... } }");
    CHECK(r.isValid());
    // frontend 不需要知道 backend 是 bgfx、platform 是 linux、profile 是 430
}

TEST_CASE(phase4_compile_thread_id_compute) {
    // 验证：compute shader 与 vertex/fragment 同样走 pool 单入口；
    // backend 选 compute 路径（pool 内部基于 IR 形状）
    pool.compile("compute Foo { let idx = thread_id.x; storage counters...; }");
}
```

**Phase 4-A / 4-B 调整**：原 §8.5.4 的 `ShaderResourcePool` 需要扩展为持有以下字段：

```cpp
class ShaderResourcePool {
public:
    // === Engine startup config ===
    void setDefaultBackend(const std::string& name);   // "bgfx"
    void setShadercExecutable(const std::string& path);
    void setBgfxIncludeDirs(std::vector<std::string> dirs);
    void setPlatform(const std::string& p);            // "linux" 等
    void setGLSLProfile(const std::string& p);         // "430" 等
    void setAutoProbeFromRendererType(bool enable);   // 默认 true
    void bindRendererType(bgfx::RendererType::Enum);  // 显式注入（可选）
    void setCacheDirectory(const std::string& dir);    // ~/.cache/ay/shaders/
    void setHotReloadEnabled(bool enable);
    void setThreadingModel(ThreadingModel m);          // Phase 4 暂 single

    // === Frontend per-call ===
    ShaderResource compile(const std::string& src);
    ShaderResource compile(const std::string& src, const CompileOptions& opts);

    // === Pool lifetime ===
    void shutdown();  // 在 bgfx::shutdown() 之前调

private:
    std::unique_ptr<ShaderResourcePoolImpl> _impl;
};
```

**Backend 注册**也并入 pool：

```cpp
// 多 backend 支持——pool 同时持有多个 backend factory
class ShaderResourcePool {
    void registerBackend(const std::string& name, BackendFactory factory);
};

// setDefaultBackend("bgfx") 实际是从已注册 backend 选默认
// Phase 8+ setDefaultBackend("hlsl") 时只切 backend name，pool 自身不动
```

**`phoskia::CompileOptions` 重设计后真正 frontend 见到**（Phase 4 后）：

```cpp
// frontend 唯一 per-call 配置：
struct CompileOptions {
    std::vector<std::string> defines;       // [variant] 编译期强制
    bool keepSources = false;
    bool dumpIntermediate = false;
    std::string dumpDir;
    // ❌ 全部 backend / 平台 / 路径 config 已下移到 pool
};

// 注：整个 `targetBackend` 字段被删——单 backend 模式下不必要，
// 多 backend 时由 pool.setDefaultBackend() 决定
```

**总结 — 用户提出的"单一入口"设计的 contract 三连**：

1. **Engine 启动**：一次调 `ShaderResourcePool::set*()` 把所有"引擎配置"配齐（backend / shaderc / platform / include / cache / hot-reload）。frontend 不再拥有这些知识。
2. **Frontend per-shader**：一次调 `pool.compile(src[, opts])`，opts 只含 frontend 该管的（defines / debug）。
3. **验证**：`Test_ShaderCacheIntegration` 测试——frontend 调用方 TU 不带 bgfx header；pool 内含 bgfx header（pimpl 隔离）。

**fronted vs backend 职责拆分（Phase 4 末）**：

| 阶段 | 谁负责 | 例子 |
|---|---|---|
| Engine startup | engine / engine-init code | `pool.setDefaultBackend("bgfx")`, `pool.setPlatform()`, `pool.setBgfxIncludeDirs()` |
| Engine runtime (per shader) | engine / game code | `pool.compile(src)` / `pool.compile(src, opts)` |
| Backend 内部 | AYShader 库 | shaderc 调用 + bgfx createShader/createProgram + cache lookup |
| Frontend-API 内部 | AYShader 库 | opaque handle / submit / 帧期 batch upload |

---

### 8.6 Structured Diagnostic API（Phase 4-N）

> **借鉴**：[rustc Diagnostic](https://rustc-dev-guide.rust-lang.org/diagnostics.html) + [Clang SourceLocation](https://clang.llvm.org/doxygen/classclang_1_1SourceLocation.html) + [TypeScript DiagnosticWithLocation](https://github.com/microsoft/TypeScript-wiki/blob/main/Architectural-Overview--Diagnostics.md)

**当前痛点**（Phase 3.6 末 / §8.4 状态）：

```cpp
struct CompilerError {
    std::string message;   // ← only a string; frontend must parse "line N:" to locate
};
```

Frontend 拿到 error 想做 "跳到编辑器第 N 行第 M 列高亮" — **必须 regex 解析 message 字符串**。这是 1970s 设计。

**Phase 4-N 目标**：

```cpp
// include/AYShaderProgram.h
namespace ayt::shader {

enum class DiagnosticSeverity : uint8_t {
    NOTE,
    WARNING,
    ERROR,
    FATAL,
};

struct SourceLocation {
    std::string sourceFile;   // 相对路径 / "<input>" / "<pool cache>"
    uint32_t    line   = 0;   // 1-based
    uint32_t    column = 0;   // 1-based（UTF-8 code unit；byte offset 也可）
    uint32_t    offset = 0;   // 0-based byte offset（备选）
};

struct PhoskiaDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::ERROR;
    std::string        message;
    SourceLocation     location;
    std::vector<SourceLocation> notes;     // related location (e.g. "first declared here")
    std::vector<std::string>     hints;    // 修复建议 (e.g. "did you mean 'vec3'?")
    std::string        errorCode;          // e.g. "E0301" - machine-readable
};

struct CompiledShaderProgram {
    // ... 已有字段
    std::vector<PhoskiaDiagnostic> errors;     // 替代 CompilerError
    std::vector<PhoskiaDiagnostic> warnings;   // 替代 std::string warnings
};

} // namespace
```

**前端消费**：

```cpp
// IDE 集成（直接消费结构体）：
for (const auto& diag : prog.errors) {
    editor.highlightRange(diag.location.sourceFile,
                         diag.location.line,
                         diag.location.column,
                         diag.severity);
    if (!diag.hints.empty()) {
        editor.showQuickFix(diag.hints.front());
    }
}

// CLI 输出（CLI 内部走 toHumanString() helper）：
// error[E0301]: 'hello' is not a builtin type
//   --> assets/unlit.phoskia:12:5
//    |
// 12 |     uniform hello x;
//    |            ^^^^^ expected: float, vec2, vec3, vec4, ...
//    |
//    = help: did you mean 'vec3'?
```

**与现有 `CompilerError` 的兼容**：

| 阶段 | 动作 |
|---|---|
| Phase 4-N | 加 `PhoskiaDiagnostic`，`CompiledShaderProgram` 同时有 `errors` (新) + `errors_legacy_` (旧 string) |
| Phase 5 | `errors_legacy` 标 `[[deprecated]]` |
| Phase 6 | 删 `errors_legacy`（按 §0 API stability，需 minor version bump） |

**§14.6.3 测试守门**：
- `Test_DiagnosticStructure`：每个错误类型至少 1 个 test case 验证 `location.line/column/sourceFile` 正确
- `Test_DiagnosticHumanFormat`：snapshot 测试 human-readable 输出
- `Test_DiagnosticJSONFormat`：可选 Phase 5+ 加 `setDiagnosticFormat(JSON)`

**风险与缓解**：

| 风险 | 缓解 |
|---|---|
| 错误信息质量下降（结构化比字符串啰嗦） | 保持 `toHumanString()` helper 默认模仿 rustc 格式 |
| 升级期 frontend 仍在用 `errors[i].message` | 双字段共存期至少 1 个 minor version |
| Location 在 parser 阶段没收集全 | 强制每条 error 都有 location（没 location 用 `SourceLocation{"<input>", 0, 0}` 兜底） |

---

### 8.7 Reflection from Phoskia AST（Phase 5+）

> **借鉴**：[SPIR-V reflection](https://github.com/google/spirv-reflect) + [DXIL reflection](https://github.com/microsoft/DirectXShaderCompiler/wiki/DXIL-Programming-Guide) + [Unreal Material Function](https://docs.unrealengine.com/5.0/en-US/material-functions-in-unreal-engine/)

**当前痛点**（Phase 3.6 末 / §8.4 状态）：

```cpp
struct CompiledShaderProgram {
    std::vector<BGFXUniformBlock> uniformBlocks;   // ← bgfx 解析的
    std::vector<BGFXStorageBuffer> storageBuffers; // ← bgfx 解析的
    std::vector<BGFXUniform>      uniforms;
    std::vector<BGFXTexture>      textures;
};
```

这 4 个 reflection 字段是 **从 shaderc 输出的 binary parse 出来的**。问题：
- 跟 Phoskia source 不一致时 frontend 不知道哪个对（"我说声明了 Lighting 但 reflection 没看到 → bug 在哪？"）
- bgfx parser 表达力有限（不会告诉我们 "这个 UBO 是 std140 layout")
- Phase 8+ 换 backend 时 reflection 要重写

**Phase 5+ 目标**：

```cpp
namespace ayt::shader::phoskia {

// 1) Reflection 结构由 Phoskia AST 直接生成（不是 binary parse）
struct ReflectionMember {
    std::string name;
    std::string type;            // "vec4" / "mat4" / "float"
    uint32_t    offset = 0;      // byte offset in block
    uint32_t    size   = 0;      // byte size
    uint32_t    arraySize = 1;   // 1 = scalar/vector/matrix; >1 = array
};

struct ReflectionBlock {
    std::string name;
    std::string layout;          // "std140" / "std430"
    uint32_t    binding = UINT32_MAX;  // explicit binding; UINT32_MAX = auto
    uint32_t    size    = 0;      // total block size
    std::vector<ReflectionMember> members;
};

struct ReflectionTexture {
    std::string name;
    std::string samplerType;     // "sampler2D" / "samplerCube" / "sampler3D" / ...
    uint32_t    binding = UINT32_MAX;
};

struct ReflectionStorageBuffer {
    std::string name;
    std::string layout;          // "std430"
    uint32_t    binding = UINT32_MAX;
};

struct Reflection {
    std::vector<ReflectionBlock>        uniformBlocks;
    std::vector<ReflectionStorageBuffer> storageBuffers;
    std::vector<ReflectionTexture>      textures;
    std::vector<ReflectionMember>       uniforms;       // non-block uniforms
};

} // namespace

// 2) CompiledShaderProgram 加 reflection 字段：
struct CompiledShaderProgram {
    // ...
    phoskia::Reflection reflection;   // ← 来自 Phoskia AST，frontend 可信
};
```

**关键 contract**：

```cpp
// Phase 5 测试：reflection 跟 shaderc binary parse 出来的 metadata 必须一致
// 不一致 → program.errors[] 加 diagnostic，frontend 知道 backend 出了问题
TEST_CASE(reflection_matches_shaderc_binary_parse) {
    // 编译同一段 source
    //   reflection_a = 来自 Phoskia AST
    //   reflection_b = 来自 shaderc 二进制 parse
    // CHECK(reflection_a.uniformBlocks == reflection_b.uniformBlocks)
}
```

**优势**：
- frontend 信任 reflection（不再需要在 binary parse 上 hack）
- Phase 8+ 换 backend 时 reflection 形态不变（来自 Phoskia AST）
- "你声明了什么" vs "shader binary 实际有什么" 不一致是 backend 的 bug，由 `AYBGFXConverter` self-check 负责

**Phase 5+ 落地路径**：
- 5-A：Reflection 数据结构 + Phoskia AST generator（替换 `BGFXUniformBlock` 等 4 个字段）
- 5-B：reflection vs shaderc binary parse 一致性 self-check（test + production 双重）
- 5-C：标 `BGFXUniformBlock` 等 4 个字段 `[[deprecated]]`
- 6-A：删 deprecated 字段

---

### 8.8 Result<T, E> API 长期目标（Phase 6+）

> **借鉴**：[Rust Result<T, E>](https://doc.rust-lang.org/std/result/enum.Result.html) + [C++23 std::expected](https://en.cppreference.com/w/cpp/utility/expected) + [Go (value, err) tuple](https://go.dev/blog/error-handling-and-go)

**Phase 4-N 的 `bool success + vector<errors>` 是过渡形态**。Phase 6+ 目标：

```cpp
// C++23：
#include <expected>
template<typename T, typename E>
using Result = std::expected<T, E>;

using CompileResult_ = Result<ShaderResource, CompileDiagnostics>;

// 调用方：
auto r = pool.compile(src);
if (!r) {
    for (const auto& d : r.error().diagnostics) {
        // IDE highlight
    }
    return;
}
auto& shader = *r;  // ShaderResource
```

**兼容路径**（Phase 6 入口）：
- 加 overload `Result<ShaderResource, CompileDiagnostics> compileResult(...)`，保留旧的 `ShaderResource compile(...)`（其内部抛 `ShaderCompileException` on error）
- Phase 7 删 overload

**风险与缓解**：

| 风险 | 缓解 |
|---|---|
| MSVC C++23 `std::expected` 未完全支持 | 用 `tl::expected` (backport) 或自己实现 lightweight `Result<T, E>` |
| Result API 改变 ABI | 走 §0 minor version bump |
| Frontend 代码 if-else 改 try-catch 改 expected 链 | 提供 migration helper（`AY_EXPECTED_VALUE` macro）|

---

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

### Phase 3.6 — 产品化收尾：`.sc` 作为 backend 内部细节 ✅ 完成（2026-07-01）

- [x] **`AYShadercDriver` 抽象**（commit `6c332b0`）— 从 `Test_ShaderCompile` plumbing 提到 production；封装 path 发现 + temp file + spawn
- [x] **`CompiledShaderProgram` + `compileToBinary()`**（commit `c2d128b`）— `.bin` bytes 走出 frontend；`.sc` 字段标 `[[deprecated]]`
- [x] **`Compiler::compileToProgram()` + env precedence**（commit `465eec6`）— 三个 overload（default / opts / out-param SSO-safe）；`AY_PHOSKIA_KEEP_SOURCES` / `AY_PHOSKIA_DUMP_SC` 双 env with true-wins OR
- [x] **shaderc 路径重设计**（commit `29db73c`，sign-off 2026-07-01）— **删除** env var / CMake hint / PATH 自动发现；改 static setter `setDefaultExecutable()` + 默认 ctor lookup。引擎一启动调一次即可。
- [x] **e2e 测试 plumbing 重写**（commit `29db73c`）— `Test_ShaderCompile.cpp` 1024 → 454 行；8 个 e2e 直接调 `compileToProgram()`
- [x] **unit-test `.output.find` 重映射 + golden joiner**（commit `a96b521`）— `Test_Phoskia` 47 个 + `Test_BGFXConverter` 1 个 + `Test_GoldenFiles` joiner 全切到 `program.sources`；`out.sources` populate 提到 shaderc 初始化之前；9 个 `.sc` baseline 不动
- [x] **env precedence + dump_dir 测试**（commit `44e6158`）— 4× `AY_PHOSKIA_DUMP_SC` precedence + `compileToProgram_respects_dump_dir`；`unsetenv` 纪律
- [x] Phase 3.6 step list 完整闭合 — 总测试 917 → **944/944 PASS**

详见 §8.4（旧版本，本文末）+ §8.5（Phase 4 衔接）。

### Phase 4: Opaque ShaderResource API + bgfx wire-up（**当前最高优先**，设计见 §8.5）

> **2026-07-01 sign-off**：shader 之后用 bgfx 接口，但**隐藏具体 API**。frontend 不再见到 `bgfx::ShaderHandle` / `bgfx::ProgramHandle` / `bgfx::UniformHandle`。

| Block | 范围 | 估算 | 依赖 |
|---|---|---|---|
| 4-A | **`ShaderResource` opaque handle class**（pimpl 隔离 bgfx types；公开 API：`getUniformBinding(name)` / `getTextureBinding(name)` / `getUniformBlockBinding(name)` / `getStorageBufferBinding(name)` / `setUniform(id, data, size)` / `setTexture(stage, id, tex)` / `submit(DrawCallContext)`） | 1.5 天 | — |
| 4-B | **`ShaderResourcePool` 工厂 + 拥有权 + 引擎配置中心**（friend 限定 ShaderResource 构造；shutdown 释放所有 handle；hot-reload dev-only hook；cache 收编；**新增**：`setDefaultBackend` / `setShadercExecutable` / `setBgfxIncludeDirs` / `setPlatform` / `setGLSLProfile` / `setAutoProbeFromRendererType` / `bindRendererType` 等 startup 配置；详见 §8.5.10） | 2 天 | — |
| 4-C | **`Compiler::compileToShaderResource(src[, opts])`**（一次拿 opaque handle；内部 compileToProgram + bgfx::createShader + bgfx::createProgram + cache lookup） | 1 天 | 4-A, 4-B |
| 4-D | **binding name → opaque ID 映射**（含 type 推断 + std140 layout 计算，把 Phase 3 的 `BGFXUniform` / `BGFXTexture` 等"前端元数据"完全收入 AYShader 内部） | 1.5 天 | 4-A |
| 4-E | **header 隔离**（确认 `include/AYShader.h` / `AYShaderProgram.h` 不再包含 `<bgfx/bgfx.h>`；改 pimpl 后只剩 `AYShaderImpl.cpp` 引用 bgfx） | 0.5 天 | 4-A |
| 4-F | **`ShaderResource::submit(DrawCallContext)` 帧期整合**（与 AYRenderer draw call 配套；包含 batch uniform upload） | 1 天 | 4-C |
| 4-G | **退役 `class ShaderProgram`**（Phase 1 老接口，Phase 4 后只作 `ShaderResourceImpl` 内部使用；frontend 不见） | 0.25 天 | 4-E |
| 4-H | **删 `.sc` 公开字段**（`BGFXShaderFiles.{vs,fs,varyingDef}` + `BGFXComputeFile::cs` + `CompileResult::output` + `ConvertResult::output`）— 原本是 Phase 3.7 的待办，并入 Phase 4 | 0.5-1 天 | 4-E |
| 4-I | **cache 重新设计**（把 `AYShaderCache` 收编进 `ShaderResourcePool::Impl`；cache key = source hash + defines hash + backend + platform + profile + includeDirs，详见 §8.5.10） | 0.75 天 | 4-B |
| 4-J | **hot-reload 实现**（dev-only，文件 mtime watch + 100ms debounce） | 0.5 天 | 4-B, 4-I |
| 4-K | **Phase 4 e2e 测试**（`Test_ShaderCacheIntegration`——real frontend 场景，取 ShaderResource → setUniform → 模拟 submit，验证全程零 bgfx header visible） | 1 天 | 4-F |
| 4-L | **🆕 `phoskia::CompileOptions` 字段精简**（删 `targetBackend` / `enableTypeInference` / `enableSemanticAnalysis`；保留 `defines` / `keepSources` / `dumpIntermediate` / `dumpDir`）；frontend-side type-checking 改由 pool 永久开启（不再 per-call 关闭） | 0.5 天 | 4-B |
| 4-M | **🆕 自动 platform / profile 探测**（首次 `pool.compile()` 时若未显式 `setPlatform`/`setGLSLProfile`，从 `bgfx::getCaps()` 读 renderer type 推 platform，再由 platform 推 profile；详见 §8.5.10） | 1 天 | 4-B |
| **总计** | — | **10-11 天** | — |

**验收 contract**（见 §8.5.7 + §8.5.10）：
1. `AYShader.h` / `AYShaderProgram.h` 不出现 bgfx::* 类型
2. `BindingId` = `uint32_t`，opaque
3. frontend 一次 `pool.compile(src)` 完成 byte-compile + bgfx wire + cache
4. frontend 调用代码不出现 backend / platform / profile / include 任何一词（编译期 grep 验证）
4. 换 WGSL backend 时 frontend 代码零改动（Phase 5+ 验证）
5. `Test_ShaderCacheIntegration` 全程零 `<bgfx/bgfx.h>` in compilation unit

**已知限制**：
- `ShaderResource::getXxxBinding(name)` 是**懒查表**（O(log n) per call），Phase 4 后 benchmark；如果 hot path 暴露就给 `compileToShaderResource` 增加预解析能力，缓存 binding ID
- 多线程 render：bgfx 本身不是 thread-safe；Phase 4 单线程 render thread 用，先不加锁；多线程后再讨论
- shader variant management（`[variant name]` 多版本预编译）Phase 4 范围内仅做"懒编译"；预编译 / pre-warm 是 Phase 5+

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
- ❌ HLSL backend（Phase 8+ 按需）
- ❌ WGSL backend（Phase 8+ 按需）

**Frontend API 形态**：
- ✅ Phase 3.6：`.sc` 从 frontend API 完全消失；`compileToProgram(src[, opts])` 返回 `CompiledShaderProgram`（raw bytes + binding metadata + debug sources）
- ✅ **Phase 4-A**：`ShaderResource` + 最小 `ShaderResourcePool::acquire(CompiledShaderProgram)` — bgfx wire-up 内化；公开头 `AYShaderResource.h` 不含 bgfx
- ✅ **Phase 4-B**：`ShaderResourcePool` 引擎配置 + `compile()` / `acquire(src)` + 内存 cache + `release()`
- ✅ **Phase 4-C**：`Compiler::compileToShaderResource(src[, opts], pool)` 一站式 compile + wire-up
- ✅ **Phase 4-E**：`AYShaderProgram.h` 不含 bgfx；legacy `ShaderProgram` 迁至 `detail/AYShaderProgramLegacy.h`
- ✅ **Phase 4-G**：`AYShader.h` 不再 include `AYShaderCache` / `AYBGFXConverter`（frontend 零 bgfx 泄漏）
- ✅ **Phase 4-K（contract）**：`Test_ShaderCacheIntegration` — frontend TU 不含 `<bgfx/bgfx.h>`
- ✅ **Phase 4-D**：std140 layout 从 Phoskia AST 字段类型计算；`ShaderResource::getUniformBlockSize` / field offset + `setUniformBlock`
- ❌ **Phase 4-F+（当前）**：Renderer e2e submit、hot-reload 完整实现、capability 体系（详见 §8.5）

**测试**：
- **944 / 944 PASS**（Phase 3.6 末状态，2026-07-01）
- ~13 测试套件覆盖 lexer / parser / IR / type inference / semantic / BGFX converter / shaderc e2e / golden file / PBR math / ShadercDriver / CompileToBinary / CompileToProgram
- 9 个 golden fixture（6 material + 2 compute + 1 with UBO binding）

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
| 3.6 | **产品化**：`.sc` 内化 + `compileToProgram` + shaderc 路径 static setter | `c41ed89` + `6c332b0` + `c2d128b` + `465eec6` + `29db73c` + `a96b521` + `44e6158` | +27 |
| 总计 | — | — | **278 → 944** |

---

### 14.2.1 Phase 3.6 commit 索引（2026-06-30 → 2026-07-01）

| Commit | 范围 | 测试增量 |
|---|---|---|
| `c41ed89` | design.md §8.4 初始设计文档 | 0 |
| `6c332b0` | Commit 1：extract `AYShadercDriver` from test plumbing | +0（旧 plumbing 保留并存） |
| `c2d128b` | Commit 2：`CompiledShaderProgram` + `compileToBinary()` | +0 |
| `465eec6` | Commit 3：`Compiler::compileToProgram()` + env-var merge | +0 |
| `29db73c` | Commit 4：shaderc 路径 static setter + `Test_ShaderCompile` plumbing rewrite | +0 |
| `a96b521` | Commit 5：`Test_Phoskia` / `Test_BGFXConverter` / `Test_GoldenFiles` 切到 `program.sources` | +21 |
| `44e6158` | Commit 6：env-var precedence tests + dump_dir test | +6 |
| **总计** | — | **917 → 944** |

---

### 14.3 待办清单（按优先级排序）

#### ✅ Phase 3.6 — 产品化收尾：`.sc` 作为 backend 内部细节（**完成**，2026-07-01，6 commits）

把 `.sc` 字符串从 frontend API 移走；shaderc 调用内化进 `AYBGFXConverter`；frontend 一次 `compileToProgram(src)` 拿到 `.bin` bytes。详见 §8.4（旧）+ §8.5（新，Phase 4 衔接）。

| Block | 范围 | 状态 |
|---|---|---|
| 3.6-A | design.md §8.4（设计文档） | ✅ `c41ed89` |
| 3.6-B1 | `AYShadercDriver` 抽象 | ✅ `6c332b0` |
| 3.6-B2 | `CompiledShaderProgram` + `compileToBinary()` | ✅ `c2d128b` |
| 3.6-B3 | `Compiler::compileToProgram()` + env-var merge | ✅ `465eec6` |
| 3.6-C1 | Test_ShaderCompile plumbing 重设计 + shaderc 路径 static setter | ✅ `29db73c` |
| 3.6-C2/3/4 | Test_Phoskia / Test_BGFXConverter / Test_GoldenFiles → `program.sources` | ✅ `a96b521` |
| 3.6-D | env-var precedence tests + cleanup | ✅ `44e6158` |
| **总计** | — | **944/944 tests pass** |

#### 🔴 Phase 4 — Opaque ShaderResource API + bgfx wire-up（**当前最高优先**，用户已 sign-off 2026-07-01）

> **设计核心**：shader 后端走 bgfx，但 frontend 永不直接见 `bgfx::*` 句柄。把现有 `Phase 4: ShaderGraph 与工具链`（位于上面 §11）旧条目**废弃并替换为**：opaque `ShaderResource` + `ShaderResourcePool` + `compileToShaderResource` 入口。详见 §8.5（含验收 contract / 风险 / 依赖）。

| Block | 范围 | 估算 | 依赖 |
|---|---|---|---|
| 4-A ✅ | **`ShaderResource` opaque handle**（pimpl；frontend API `get*Binding` / `setUniform` / `setTexture` / `submit`） | 1.5 天 | — |
| 4-B | **`ShaderResourcePool`**（工厂 + 拥有权 + cache 收编） | 1.5 天 | — |
| 4-C | **`Compiler::compileToShaderResource(src[, opts])`**（一站式 compile + bgfx wire + cache） | 1 天 | 4-A, 4-B |
| 4-D | **binding name → opaque ID 映射**（含 type 推断 + std140 layout） | 1.5 天 | 4-A |
| 4-E | **header 隔离**（`AYShader.h` / `AYShaderProgram.h` 不再 include `<bgfx/bgfx.h>`；pimpl 唯一下沉到 `AYShaderImpl.cpp`） | 0.5 天 | 4-A |
| 4-F | **`ShaderResource::submit(DrawCallContext)` 整合**（与 AYRenderer draw call 配套；batch upload） | 1 天 | 4-C |
| 4-G | **退役 `class ShaderProgram`**（Phase 1 老接口；frontend 永不见） | 0.25 天 | 4-E |
| 4-H | **删 `.sc` 公开字段**（`BGFXShaderFiles.{vs,fs,varyingDef}` + `BGFXComputeFile::cs` + `CompileResult::output` + `ConvertResult::output`；原本属 Phase 3.7） | 0.5-1 天 | 4-E |
| 4-I | **cache 重新设计**（`AYShaderCache` 收编进 `ShaderResourcePool::Impl`；cache key = source hash + opts hash + backend） | 0.75 天 | 4-B |
| 4-J | **hot-reload 实现**（dev-only，文件 mtime watch + 100ms debounce） | 0.5 天 | 4-B, 4-I |
| 4-K | **Phase 4 e2e 测试**（`Test_ShaderCacheIntegration`——零 `<bgfx/bgfx.h>` 在 compilation unit） | 1 天 | 4-F |
| 4-L | **`ShaderResourcePool::require(ShaderCapability)`**——capability-based backend 选择（替代 string-based `setDefaultBackend("bgfx")`；frontend 描述需求而非 backend 名） | 1 天 | 4-B |
| 4-M | **自动 platform / profile 探测** from `bgfx::getCaps()`（pool 构造时若未显式 `setPlatform`，自动选 renderer-type → (platform, profile)） | 0.75 天 | 4-B |
| 4-N | **结构化 diagnostic**（`CompilerError` → `PhoskiaDiagnostic { Severity, Message, Line, Column, SourceFile, Hint? }`；IDE 可直接消费） | 1 天 | 4-C |
| 4-O | **opaque handle ABI 收紧**（`ShaderResource` 唯一公开字段 `uint64_t _id`；frontend 可随便拷贝/移动，pool 内部 LRU 释放不失效） | 0.5 天 | 4-A |
| 4-P | **source key 改名**（`vs_<i>.sc` → `vertex_stage_<i>`；frontend 命名空间永不见 `.sc` 后缀） | 0.5 天 | 4-H |
| 4-Q | **two-tier cache**（source cache `SHA256(source)` → `IRProgram`；binary cache `SHA256(src+defines+capability+platform+profile+include)` → bytes；分层 invalidation） | 1 天 | 4-I |
| 4-R | **`phoskia::CompileOptions` 字段精简**（删 backend-internal 字段；前端选项只剩 `defines / keepSources / dumpIntermediate / dumpDir` + debug toggles；详见 §8.5.10） | 0.5 天 | 4-L, 4-M |
| **总计** | — | **14-17 天** | — |

> **§14.3 与 §8.5 关联**：4-L / 4-M / 4-N / 4-O / 4-P / 4-Q / 4-R 都是 §8.5 主体设计在 phase 表格里的"实施切片"。其中：
> - **4-L + 4-M + 4-R** 一并落地才能真正做到 §8.5.10 的 frontend "封顶"（frontend 不出现 backend / platform / profile / include 任何一词，编译期 grep 验证）
> - **4-O** 是 §8.5.4 的 ABI 收紧；不依赖其他 block，可并行
> - **4-P** 是 §8.4 的 source key 演进；必须在 4-H 删字段时同步
> - **4-Q** 是 §8.5 的 cache 重设计；与 4-I 互补（4-I 是 cache 收编，4-Q 是 cache 分层）

#### 🟡 Phase 5 — Compute + Texture 补完（覆盖 90% 真实项目需求）— **原 Phase 3.7，降级**

> **2026-07 小步切片（已落地）**：`dFdx`/`dFdy`/`fwidth` 内置 + `texturecube`/`SAMPLERCUBE`/`textureCube`；其余项标注 🅿 延后。

| # | 能力 | 表面语法 | emit | 估算 | 依赖 | 状态 |
|---|---|---|---|---|---|---|
| 1 | **Storage image** | `storageimage X : rwimage2d<float> binding N;` | `layout(r32f, binding = N) uniform image2D X;` + `imageLoad / imageStore` 内置 | 1.5 天 | — | 🅿 |
| 2 | **多 texture kind** | `texturecube envMap;` / `texture3d noise;` / `texture2darray lookup;` | `SAMPLERCUBE / SAMPLER3D / SAMPLER2DARRAY` 宏 | 2 天 | — | **cube ✅**；3d/array 🅿 |
| 3 | **Compute barrier** | `barrier();` `memoryBarrierShared();` 内置 | `barrier();` `memoryBarrierShared();` | 0.5 天 | — | 🅿 |
| 4 | **原子操作** | `atomicAdd(ptr, val);` `atomicMin(...);` 等内置 | `atomicAdd(ptr.data[idx], val);` 等 | 1 天 | — | 🅿 |
| 5 | **Fragment 导数** | `dFdx(v) / dFdy(v) / fwidth(v)` 内置 | `dFdx / dFdy / fwidth` | 0.25 天 | — | ✅ |

合计：~5 天。**打开 PBR 后处理 / 流体模拟 / GPGPU 通用计算 / 法线贴图等关键场景**。

#### 🟡 Phase 6 — 高级渲染特性（覆盖剩余 10%）— **原 Phase 3.8，降级**

| # | 能力 | 表面语法 | emit | 估算 | 依赖 | 状态 |
|---|---|---|---|---|---|---|
| 6 | **MRT（多 render target）** | `out color1 : color = vec4(0.0);` 声明序 → slot | `gl_FragData[0..7] = ...;` | 1.5 天 | — | ✅ |
| 7 | **Vertex 多 attribute** | `in tan : tangent;` / `in uv2 : texcoord1;` | 扩 `semanticTable()` | 1 天 | — | — |
| 8 | **Indirect dispatch/draw** | `dispatchIndirect(buf, off, x, y, z);` builtin | bgfx `dispatchIndirect` 宏 | 1 天 | 候选 1 | — |
| 9 | **Integer sampler** | `texture2di lookup;` / `texture2du lookup;` | `ISAMPLER2D / USAMPLER2D` | 0.5 天 | 候选 2 | — |
| 10 | **Shadow sampler** | `texture2dshadow shadowMap;` | `SAMPLER2DSHADOW` + PCF 内置 | 0.5 天 | 候选 2 | — |

合计：~4.5 天。**打开 deferred shading / G-buffer / 高级 PBR / GPU-driven culling 等场景**。

#### 🟠 Phase 7 — 工程质量（跨阶段）— **原 Phase 3.8 工程质量块，降级**

| # | 能力 | 估算 | 依赖 |
|---|---|---|---|
| ~~11~~ | ~~**UBO 用户显式 binding**（`uniformblock Camera { ... } binding 0;`）~~ —— **Phase 3.5-B 已做**（commit 063664f） | — | — |
| 12 | **UBO field strict type-check**（注册 block 为 StructType，inferMemberExpr 支持 struct） | 1.5 天 | 候选 14 |
| 13 | **SSA IR + 跨后端优化**（constant folding / DCE） | 7-10 天 | — |
| 14 | **struct 类型系统**（`struct Light { vec3 dir; vec3 color; }`） | 3-4 天 | 候选 12 |

合计：~12-15 天。**让 Phoskia 达到生产级 shader DSL 水平**。

#### 🔵 Phase 8+ — 跨后端（按需启动，仅在项目要求时）

| # | 能力 | 触发条件 | 估算 | 备注 |
|---|---|---|---|---|
| 15 | HLSL backend（`AYHLSLConverter`，emit HLSL 6.x） | 项目要求 DXC 一手质量 / 摆脱 shaderc | 5-7 天 | Phase 4 的 `ShaderResourcePool` 已经抽象掉 bgfx，Phase 8 加 HLSL 时**只换** `ShaderResourceImpl` 内部实现 |
| 16 | WGSL backend（`AYWGLSConverter`，emit WGSL） | WebGPU 目标 + 切 runtime 到 wgpu-native / Dawn | 7-10 天 | 同上—frontend 代码不改 |
| 17 | Ray tracing shader | DXR / Vulkan RT 需求（需新 backend） | TBD | — |

> **§14.3 与 §8.5 的 design 关联**：Phase 4 完成后，Phase 8+ 的核心工作只是**替换 `ShaderResourceImpl`**——frontend API（`ShaderResource` / `BindingId` / `compileToShaderResource`）已与 backend 解耦。这意味着 Phase 8 不需要碰 frontend，不需要大改 `AYShader.h`。

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
- **bgfx 头文件隔离（Phase 4 起）**：所有 bgfx handles 必须 pimpl 隔离，详见 §8.5。Frontend 头文件（`AYShader.h` / `AYShaderProgram.h`）**禁止**直接 `#include <bgfx/bgfx.h>`——编译单元验证见 `Test_ShaderCacheIntegration`。

### 14.5 Phase 4 起路线图（移交用，一页摘要）

**核心 contract**：frontend 只见 opaque handle；bgfx 永远在 pimpl 里。

```
                          ┌───────────────────────────┐
                          │  AYShader (公共 API)      │
                          │  - ShaderResource         │  ← opaque handle
                          │  - ShaderResourcePool     │  ← 工厂 + 拥有权
                          │  - BindingId / TextureHandle│  ← opaque numeric / driver-agnostic
                          └─────────┬─────────────────┘
                                    │ pimpl
                          ┌─────────▼─────────────────┐
                          │  AYShaderImpl.cpp          │
                          │  - bgfx::ShaderHandle      │
                          │  - bgfx::ProgramHandle     │  ← 唯一允许见 bgfx headers
                          │  - bgfx::UniformHandle     │    的 TU（编译单元隔离）
                          │  - bgfx::setUniform(...)/ submit
                          └─────────┬─────────────────┘
                                    │
                          ┌─────────▼─────────────────┐
                          │  AYBGFXConverter           │  ← Phoskia → .sc → bytes
                          │  AYShadercDriver           │  ← bytes → spawn shaderc
                          └───────────────────────────┘
```

**Phoskia 公开 API 表面（Phase 4 后的目标）**：

```cpp
// === frontend 唯一入口 ===
ayt::shader::ShaderResourcePool shaderPool;       // engine startup
ShaderResource res = phoskia.compileToShaderResource(src);  // 一站式
BindingId mvp = res.getUniformBinding("modelViewProj");
res.setUniform(mvp, &matrix, sizeof(matrix));
res.submit(drawCtx);

// frontend 代码不再含 #include <bgfx/bgfx.h>
```

**Phase 4 → Phase 8+ 替换路径**：

| Phase | 工作 | 改 frontend API 吗？ |
|---|---|---|
| **4** | `ShaderResource` + Pool + bgfx wire-up | ❌ 一次性定下 API，Phase 8 之后**不再改 frontend** |
| 4-H | 删 `.sc` 公开字段 | ❌ 同上 |
| 5 / 6 / 7 | Phoskia 语法扩展 | ❌ **不**影响 frontend API（只扩展 `CompileOptions` / `compiled.sources` 形态） |
| **8+（HLSL）** | 写新 `AYHLSLConverter` + 写新 `ShaderResourceImpl`（含 DXC 集成） | ❌ **frontend 零改动**——`ShaderResource` 抽象已与 backend 解耦 |
| **8+（WGSL）** | 同上（runtime 切到 wgpu-native / Dawn） | ❌ 同上 |

**这意味着**：Phase 4 是 frontend API 的"封顶 commit" — 通过之后，每加 backend / feature 都属于"扩展"而非"破坏"。

**Phase 4 是否成功验收（§8.5.7）复述**：
1. `AYShader.h` / `AYShaderProgram.h` 不出现 bgfx::* 类型
2. `BindingId = uint32_t` opaque
3. frontend 一次 `compileToShaderResource(src)` 完成 byte-compile + bgfx wire + cache
4. 换 WGSL backend 时 frontend 代码零改动（Phase 8+ 验证）
5. `Test_ShaderCacheIntegration` 全程零 `<bgfx/bgfx.h>` 在 compilation unit

---

### 14.6 Design Review Checklist（每次 PR 跑一遍）

> **目的**：避免"想起一个改一个"的被动设计。借鉴 [Rust RFC checklist](https://github.com/rust-lang/rfcs) + [LLVM Developer Policy](https://llvm.org/docs/DeveloperPolicy.html) + [TypeScript Design Notes](https://github.com/microsoft/TypeScript/wiki/TypeScript-Design-Notes)。

每次提交 PR 前，**所有 reviewable 项必须勾选**。未勾选项要么 commit 说明里 explain why，要么进 §14.7 RFC 流程。

#### 14.6.1 API surface（lock-in 类）

- [ ] **Frontend header 不出现 bgfx**：`grep -rn 'bgfx/' include/AYShader.h include/AYShaderProgram.h include/AYPhoskia.h` 应为空（4-E 之后永久成立）
- [ ] **`ShaderResource` 不含 `bgfx::*Handle` 字段**：`grep -rn 'bgfx::' include/AYShaderProgram.h` 应为空（4-O 之后）
- [ ] **`phoskia::CompileOptions` 不出现 backend / platform / profile / include / targetBackend 等 backend-internal 字段**（4-R 之后）：
  ```bash
  grep -E '(targetBackend|targetPlatform|glslProfile|includeDir)' include/AYPhoskia.h
  ```
- [ ] **新加字段是 default-constructed 类型**（`std::optional` / `std::vector` / default 值），**不**重排已有字段
- [ ] **新增函数是 overload**，不改已有签名

#### 14.6.2 Cache 一致性

- [ ] **Cache key 五字段没新加的**：source / defines / capability / platform / profile / includeDirs。**新加字段要更新 cache key + 同步 §8.5**（4-Q 之后）
- [ ] **Invalidation 规则**没破：source 改 → IR 改 → binary 重编；defines 改 → binary 重编；backend 改 → binary 重编

#### 14.6.3 错误处理

- [ ] **错误走 `program.errors[]`**，不抛异常到 frontend（§15.1 SSO 教训）
- [ ] **`CompilerError` 字段含 line/column**，IDE 可定位（4-N 之后）

#### 14.6.4 测试覆盖

- [ ] **每个新增 public API 有对应 unit test**
- [ ] **Frontend 公开 API 全 surface 已覆盖**（`Test_HeaderSurface` 在 Phase 5+ 加）
- [ ] **ABI sizeof 锁定**（4-K 测试夹具）

#### 14.6.5 文档同步

- [ ] **`design.md` 对应章节同步更新**（§8 / §10 BNF / §11 phase 计划）
- [ ] **`README.md` status 表 + 当前 phase 状态同步**
- [ ] **CHANGELOG** 列出新增/标 deprecated/删除项（§0.4）

#### 14.6.6 性能预算（Phase 5+ 强制）

- [ ] **冷启动编译 100-material PBR shader** ≤ Phase 上次 baseline × 1.1
- [ ] **memory 占用** ≤ Phase 上次 baseline × 1.1
- [ ] **cache hit ratio** ≥ 95%（CI 测试）

---

### 14.7 RFC-pending 流程（Phase 4+ 每个新 feature 走一遍）

> **目的**：让设计主动前置。借鉴 Rust RFC process（rust-lang/rfcs）、TypeScript Design Notes、Ember RFC（emberjs/rfcs）。

#### 14.7.1 什么时候走 RFC

| 触发 | 是否需要 RFC |
|---|---|
| 加新 public API（struct / function / enum 值） | ✅ 必须 |
| 改 `phoskia::CompileOptions` 字段 | ✅ 必须 |
| 加新 backend（HLSL / WGSL）| ✅ 必须 |
| 改 cache key 字段 | ✅ 必须 |
| 改 ABI / sizeof 已锁定 struct | ❌ 走 §0 major bump |
| 内部重构（不改 public API） | ❌ 不需要 |
| 加新 test fixture | ❌ 不需要 |
| 修 bug | ❌ 但要在 commit message 写 root cause |

#### 14.7.2 RFC 模板（design.md 单节）

```markdown
### §X.Y RFC-<编号>: <feature 名字>

**Status**: RFC-Pending → RFC-Accepted → RFC-Implemented → RFC-Stabilized
**Date**: <创建日期>
**Author**: <作者>
**Sign-off**: <user sign-off 日期>

## Motivation
<为什么需要这个 feature / 痛点是什么>

## Design
<API 表面 / 数据结构 / 行为>

## Alternatives Considered
<考虑过的替代方案 + 为什么否决>

## Risks
<风险 + 缓解>

## Test Plan
<怎么验证>

## Acceptance Contract
<可机器验证的成功标准>

## Open Questions
<未决项>
```

#### 14.7.3 RFC 生命周期

| 阶段 | 动作 | 状态字段 |
|---|---|---|
| 1. RFC-Pending | 写 §X.Y 草案 | "RFC-Pending" |
| 2. Self-review | 自己跑一遍 §14.6 checklist | "RFC-Pending → Reviewing" |
| 3. User-review | 邀请 user 在 chat / PR review 看 | "RFC-Reviewing → Accepted" |
| 4. Implementation | 按 RFC 写代码 | "RFC-Accepted → Implemented" |
| 5. Stabilization | 通过 §14.6 checklist + 测试全 pass | "RFC-Implemented → Stabilized" |

**未 RFC-Accepted 不允许写代码**。这是硬约束——避免"先写再说"的设计腐败。

#### 14.7.4 RFC 索引（持续累积）

| 编号 | 标题 | 状态 | 关联 phase |
|---|---|---|---|
| RFC-001 | Opaque ShaderResource API | Stabilized（§8.5） | Phase 4-A..K |
| RFC-002 | Engine-driven config consolidation | Stabilized（§8.5.10） | Phase 4-L, 4-M, 4-R |
| RFC-003 | Structured diagnostic | Accepted（§8.6） | Phase 4-N |
| RFC-004 | Opaque handle ABI 收紧 | Accepted（§8.5.4） | Phase 4-O |
| RFC-005 | Source key 改名（脱 `.sc`） | Accepted（§8.4） | Phase 4-P |
| RFC-006 | Two-tier cache | Accepted（§8.5） | Phase 4-Q |
| RFC-007 | Reflection from Phoskia AST | Accepted（§8.7） | Phase 5 |
| RFC-008 | Result<T, E> API | Pending（§8.8） | Phase 6+ |
| RFC-009 | CompileOptions builder pattern | Pending（§8.4） | Phase 6+ |
| RFC-010 | API Stability Promise | Stabilized（§0） | Phase 4 入口 |
| RFC-011 | File I/O via AYIO | Accepted（§16） | Phase 4+ |
| RFC-012 | AYIO::Process 模块（shaderc spawn 统一化） | Pending | Future Work |
| RFC-013 | env::get / env::contains on AYIO | Accepted（§16） | Phase 4+ |

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

### 15.5 shaderc 路径 global state（Phase 3.6 决策记录）

**问题演进**：Phase 1 / Phase 2 把 shaderc 路径硬编码到 `Test_ShaderCompile.cpp` plumbing 里，靠 env var `AY_SHADER_SHADERC` + CMake hint `AY_SHADER_SHADERC_HINT` + PATH 搜 shaderc.exe 三层 fallback 解决。**实测在该机器上全部失败**（`where shaderc` 不返回 vendored binary 路径），用户 sign-off：移除 auto-discovery，强制 frontend 主动配置。

**决策**：process-wide static setter。`AYShadercDriver::setDefaultExecutable(path)` 在引擎启动调一次；之后所有 default-constructed `AYShadercDriver` 都用这个路径。

**为什么是 static**（不是 per-Compiler / per-Pool 字段）：
1. 一个 engine process 一个 shaderc 路径（无需 per-thread/per-pool override）
2. 测试隔离用 `clearDefaultExecutable()` per-test setup
3. Meyers singleton + `std::shared_ptr<const std::string>`：读取无锁，写入 atomic ref-bump（thread-safe 读不撕裂）

**风险与缓解**：
- 测试 cross-pollution：每个测试开头 `clearDefaultExecutable()` + `shadercReachable()` 重新 probe；这是 §15.2 之外的额外纪律
- per-call override：仍然支持 `BGFXCompileOptions::shadercPath`（空字符串=用 global），给特殊场景留 escape

### 15.6 frontend API 形态演化（Phase 1 → 3.6 → 4）

| Phase | frontend 见到 | binding 见到 |
|---|---|---|
| Phase 1 | `.sc` 字符串 + 自己 spawn shaderc + 自己调 bgfx | 直接持有 bgfx handles |
| Phase 3.6 | `.bin` bytes + binding metadata；frontend 自己调 bgfx | 持有 `bgfx::*Handle` 但**完整元数据结构体已显化** |
| **Phase 4（目标）** | opaque `ShaderResource` + `BindingId` | frontend 永不见 bgfx |

设计原则：**frontend API 只追加不破坏**。Phase 4 把 frontend API "封顶"，之后再加 backend / feature 都是扩展。

---

### 15.7 Two-tier cache 的设计教训（Phase 4-Q 决策记录）

**问题演进**：
- Phase 1 cache 设计：完全没有 cache，每次 compile 都跑 lexer + parser + IR + backend + shaderc
- Phase 2-3.6 cache 设计：单层 binary cache，key = `SHA256(src+defines+backend)`（详见 §9）
- Phase 4-I 计划：cache 收编进 `ShaderResourcePool::Impl`
- **问题暴露**（Phase 4-Q 触发）：单层 binary cache 的几个设计缺陷在 hot-reload 测试里冒出来

**缺陷 1：source cache 缺失**

frontend 调试改 `src` 想看 AST 错误（如 "你没声明 foo"），cache hit 直接拿 binary，**没有 AST 错误信息**——frontend 误以为编译成功了。

**缺陷 2：defines 改了全栈重跑**

defines 改了 → cache miss → 重新跑 lexer + parser + IR + backend + shaderc。前 3 层是浪费——IR 不依赖 defines。

**缺陷 3：incremental compile 不可能**

frontend 想做"只改了一个 material → 只重编这一个"。当前 cache key 是 source 全 hash，没法定位到 fragment。

**决策**：two-tier cache。详见 §8.5.5.1。

| Tier | Key | Value | 失效时机 |
|---|---|---|---|
| 1 | `SHA256(source)` | `IRProgram`（AST + 类型化 IR + reflection） | source 改 |
| 2 | `SHA256(source + defines + capability + platform + profile + includeDirs)` | `.bin` bytes + bgfx handle | defines/capability/platform/profile/includeDirs 改 |

**教训（写入 §14.6.2 checklist）**：
- **新加字段必须更新 cache key**：每次给 `CompileOptions` / pool config 加字段，**自动检查 cache key 是否包含新字段**
- **cache 测试覆盖 invalidation 矩阵**：所有字段变体都要有 test（改 defines 后 tier 2 miss 但 tier 1 hit，等等）
- **frontend 调试永远有 AST 错误信息**：tier 1 cache miss 时，frontend 拿到的不是空 `program`，而是**带 errors 的 program**（即使 binary 没编出来）

**借鉴**：[Unreal DDC 分层](https://docs.unrealengine.com/5.0/en-US/derived-data-cache-in-unreal-engine/) + [V8 code cache 分层](https://v8.dev/blog/code-coverage) + [Rust incremental compilation](https://rustc-dev-guide.rust-lang.org/queries/incremental-compilation.html)

---

### 15.8 Reflection from AST vs binary parse 的设计教训（Phase 5+ 决策记录）

**问题演进**：
- Phase 1-3.5：`CompiledShaderProgram` 不暴露 binding 元数据，frontend 自己从 shader binary parse
- Phase 3.6：加 `BGFXUniformBlock` / `BGFXStorageBuffer` / `BGFXUniform` / `BGFXTexture` 4 个字段——**从 shaderc 输出 binary parse 出来**（详见 §8.4 / §8.7）
- **问题暴露**（Phase 4 调研 binding 系统时）：
  1. Phoskia source 声明 `uniformblock Camera binding 0` + std140 layout，shaderc binary parse 出来**丢失 layout 信息**（parser 只看到 byte layout，不看源码意图）
  2. 调试时 frontend 改 source，reflection 不一致——"我说声明了 Lighting 但 reflection 没看到 → bug 在哪？"
  3. Phase 8+ 换 backend（HLSL / WGSL）时这 4 个字段的 parse 逻辑要重写

**决策**：reflection 直接来自 Phoskia AST，不来自 binary parse。详见 §8.7。

**关键 contract**：
- frontend 信任 reflection（来自 AST = user 写的 = ground truth）
- backend 仍可以 self-check：AST reflection vs binary parse 不一致 → `program.errors[]` 加 diagnostic
- Phase 8+ 换 backend 时 reflection 形态不变（始终来自 Phoskia AST）

**教训（写入 §14.6 checklist）**：
- **reflection source of truth 永远来自 Phoskia AST**，不来自 backend binary
- **任何"从 binary parse metadata"的设计都要 explicit 标注** + 写明为啥不能从 AST 来
- **Phase 8+ 加新 backend 时不重写 reflection 生成代码**——AST 一次生成，多 backend 共享

**借鉴**：[SPIR-V reflection](https://github.com/google/spirv-reflect)（"reflection = separate from binary"）+ [DXC DXIL reflection](https://github.com/microsoft/DirectXShaderCompiler/wiki/DXIL-Programming-Guide) + [Unreal Material reflection](https://docs.unrealengine.com/5.0/en-US/material-editor-how-to-reflect-specular-and-roughness.html)（从 material graph AST 来，不从 HLSL parse）

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

---

## 16. File I/O via AYIO（RFC-011 / RFC-013）

### 16.1 范围与决策

AYShader 的 production `src/` 通过 `ayt::io`（AYFoundation/AYIO）收敛所有 file I/O，彻底删除 `std::ifstream` / `std::ofstream` / `::stat` / `::remove` / `mkdir` / `_mkdir` / `GetFileAttributesExA` 的手写实现。

**迁移范围**：6 个 TU
- `src/AYShaderDiskCache.cpp`（cache 读写）
- `src/AYShaderFileWatch.cpp`（hot-reload mtime）
- `src/AYShaderResourcePool.cpp`（cache key 删除）
- `src/AYShadercDriver.cpp`（shaderc 临时文件）
- `src/AYBGFXConverter.cpp`（dumpIntermediate 写盘）
- `src/AYPhoskia.cpp`（env-var 读取）

**决策**：
- 文件读写主路径用 `ayt::io::File` (raw `read`/`write`) + `queryAttributes` 预 reserve；增加 `File::readAllText` / `readAllBytes` / `writeAllText` / `writeAllBytes` 四个便利函数（RFC-011 配套扩展）
- 整文件读（.aysc cache、.phoskia 源）用 `ayt::io::MemoryMappedFile`（零拷贝）
- 临时文件用 `ayt::io::TempFile::tempDir()` 探测系统 temp 目录，**保留手写的 pid_counter 命名以维持 .sc 后缀**（见 §16.5）
- 写 cache / dump .sc 用 `ayt::io::File::writeAllText`（dump 是 best-effort，不需要 atomicWrite 的额外开销）
- env-var 读取统一走 `ayt::io::env::get` / `env::contains`（RFC-013 新增 API）

### 16.2 替换映射表（关键 11 处）

| 调用点 | 旧实现 | 新实现 |
|---|---|---|
| `AYShadercDriver.cpp::fileExists` | `::stat` | `ayt::io::File::exists` |
| `AYShadercDriver.cpp::uniqueScTempPath` | `std::getenv("TEMP"/"TMPDIR")` + 手写 fallback | `ayt::io::TempFile::tempDir()` + `path::join` |
| `AYShadercDriver.cpp::compile`（写 .sc） | `std::ofstream` + RAII `::remove` cleanup | `ayt::io::File::writeAllText` + `File::remove` |
| `AYShadercDriver.cpp::compile`（读 .bin） | `std::ifstream` + `istreambuf_iterator` | `ayt::io::MemoryMappedFile` |
| `AYShaderDiskCache.cpp::loadCompiledProgram` | `std::ifstream` | `MemoryMappedFile` + `std::stringstream` |
| `AYShaderDiskCache.cpp::saveCompiledProgram` | `std::ofstream(trunc)` | `File::atomicWrite`（write-temp-then-rename，防 crash 残留 truncated .aysc）|
| `AYShaderFileWatch.cpp::fileMtimeMs` | `GetFileAttributesExA` / `::stat` | `File::lastModifiedTime` × 1000（见 §16.4）|
| `AYShaderFileWatch.cpp::readTextFile` | `std::ifstream` + seekg/tellg | `File::readAllText` + `File::queryAttributes` 兜底 |
| `AYBGFXConverter.cpp::dumpScFile` | `std::ofstream` + `dir + "/" + key` 拼接 | `File::writeAllText` + `path::join` |
| `AYShaderResourcePool.cpp::Impl::eraseCacheKey` | `std::remove` | `File::remove` |
| `AYPhoskia.cpp::applyEnvOverrides` | `std::getenv` | `ayt::io::env::get` |

### 16.3 不在范围（Future Work）

- **进程 spawn**：`CreateProcessW` / `popen` 仍保留在 `AYShadercDriver.cpp::spawnCapturing`。这是 OS process API，不是 file I/O。AYIO 当前**未**暴露 Process 模块。**RFC-012**：`ayt::io::Process` 命名空间（spawn / capture / kill / env-block），把 AYShadercDriver 也迁过去。跟本任务解耦，单独 phase。
- **unittest/ 内 file I/O**：不在本任务范围（user explicitly excluded）。
- **shaderc 输出 .bin 大文件零拷贝**：当前 `MemoryMappedFile` → `vector<uint8_t>(ptr, ptr+sz)` 仍然有一次拷贝；如需彻底零拷贝可让 `CompiledShaderProgram` 持有 `MemoryMappedFile` handle，与 handle 生命周期绑定——单独 RFC。

### 16.4 mtime 单位语义转换

AYShader 旧实现返回 `int64_t` 毫秒（POSIX 用 `st_mtim.tv_nsec / 1e6`）。
AYIO `File::lastModifiedTime(const std::string& path)` 静态版本返回 `uint64_t` Unix 秒。

**简化决策**：取秒精度（×1000）。hot-reload 检测差异 ms 级无意义——文件修改是秒级事件，ms 精度不增加分辨力。**损失**：POSIX 上 ms 内多次修改无法分辨；**收益**：代码简化 + 跨平台一致（Win32 `GetFileAttributesExA` 返回的 FILETIME 本来就是 100ns 单位，但精度等同于秒）。

### 16.5 临时文件后缀（.sc）决策

旧实现 `uniqueScTempPath` 输出 `ayshader_<pid>_<counter>.sc`，带 `.sc` 后缀便于人类肉眼识别 / 调试。

新实现**保留手写路径生成**（不走 `TempFile::create` RAII）以维持 `.sc` 后缀：
```cpp
const std::string dir = ayt::io::TempFile::tempDir();
return ayt::io::path::join(dir,
    "ayshader_" + std::to_string(pid) + "_" + std::to_string(n) + ".sc");
```

**为什么不直接用 TempFile RAII**：`ayt::io::TempFile::create(dir, prefix)` 生成的路径没有固定后缀（AYIO 当前不暴露后缀控制）。shaderc 不在意扩展名（看 `--type` 参数），但**调试体验**需要肉眼识别 `ayshader_*.sc`。

**未来扩展路径**：如需要 RAII + 后缀，扩展 AYIO `TempFile` 加 `createWithSuffix(dir, prefix, suffix)` 或 `setExtension`；届时可以收回手写路径生成。

### 16.6 AYIO 配套扩展（RFC-011 / RFC-013）

本任务同步在 AYIO 增加 4 + 2 个 API：

**`File` 类新增静态便利函数**（`include/ayio/File.h`，`src/AYFile.cpp`）：
```cpp
static std::string         readAllText(const std::string& path);
static std::vector<uint8_t> readAllBytes(const std::string& path);
static bool                writeAllText(const std::string& path, const std::string& text);
static bool                writeAllBytes(const std::string& path, const std::vector<uint8_t>& bytes);
static uint64_t            lastModifiedTime(const std::string& path);  // RFC-011 配套扩展
```

**`Directory` 类补全静态实现**（`src/AYDirectory.cpp`）：
```cpp
static bool createRecursive(const std::string& path);  // 已在 AYDirectory.h:151 声明但缺实现，补全
```

**`ayt::io::env` 命名空间新增**（`include/AYEnv.h`，`src/AYEnv.cpp`）：
```cpp
namespace ayt::io::env {
    std::optional<std::string> get(const std::string& name);
    bool contains(const std::string& name);
}
```

**语义契约**：
- `readAllText/Bytes` 缺失文件 → 空 string / 空 vector（caller 用 `.empty()` 判定）
- `writeAllText/Bytes` 写失败 → false（不抛异常，与 `File` 类一致）
- `static lastModifiedTime(path)` 文件缺失或路径空 → 0（与 instance method 关闭 handle 时返回 0 一致；caller 用 `t > 0` 判定有效）
- `env::get` 未设 → `std::nullopt`；设为空字符串 → `Some("")`（与 `std::getenv` 语义一致）
- `env::contains` 未设 → false；设为空字符串 → true（变量**存在**但值为空）

**为什么单独加 `static lastModifiedTime(path)` 而不是让 caller 用 `File::lastModifiedTimePoint(path)`**：
- `lastModifiedTimePoint(path)` 已经存在但返回 `unique_ptr<ITimePoint>`，调用方还要 `.toUnixMs()` 拿 ms；多一次间接 + 一次堆分配
- 直接返回 `uint64_t Unix 秒` 对 hot-reload 检测已经足够（秒级精度，见 §16.4）
- 实现走 `stat()` / `GetFileAttributesExA`，比 open-then-close-File 的实例方法少一对 syscall

**测试覆盖**（在 AYIO 自带 unittest，**不在** AYShader unittest）：
- `readAllText_writes_round_trip` / `readAllBytes_writes_round_trip` / `readAllText_missing_file_returns_empty` / `writeAllText_empty_string_truncates`
- `lastModifiedTime_static_by_path_returns_nonzero` / `lastModifiedTime_static_missing_file_returns_zero` / `lastModifiedTime_static_empty_path_returns_zero`
- `directory_createRecursive_static_creates_nested` / `directory_createRecursive_static_returns_false_when_already_exists`（防止 declared-but-not-defined 再发生；锁定非幂等语义）
- `env_contains_set_var_returns_true` / `env_contains_set_to_empty_returns_true`（Win32 `_putenv_s(name, "")` 实际是 unset；POSIX `setenv(name, "", 1)` 是 set-to-empty — 测试按平台分流）

**Linker-error 教训**：本任务第一次 build 报 `LNK2019 ayt::io::Directory::createRecursive`，根因是 `Directory::createRecursive(path)` **已在 `AYDirectory.h:151` 声明但无对应实现**——属于 AYIO 自身的 declared-but-not-defined bug（之前只有实例版本被实现，静态版本是空头支票）。Free function `ayt::io::createDirectory(path)` 已存在并走相同的递归 mkdir 路径。修复方法：在 `src/AYDirectory.cpp` 补 4 行 delegate 实现，让 `Directory` 类 API 自洽。后续 AYResource / AYFont 走同样的静态 API 不会再踩坑。
- `env_get_returns_value_when_set` / `env_get_returns_nullopt_when_unset` / `env_contains_set_var_returns_true` / `env_contains_unset_var_returns_false`

### 16.7 CMakeLists 改动

只有 1 处：
```diff
# D:/Projects/AYRuntime/AYShader/CMakeLists.txt
 target_link_libraries(AYShader PUBLIC
+    AYIO
     bgfx::bgfx
 )
```

AYIO 自身 PUBLIC 依赖 AYTime + AYString，无需显式 link。`add_subdirectory(AYFoundation/AYIO)` 已在根 `CMakeLists.txt:31` 排好。

### 16.8 设计教训（§15.x 候选）

**自实现 file I/O 的成本被低估了 5 年**。原作者写 `::stat` 是为了"避免 std::filesystem 名字冲突"，结果代码里到处是同一段 stat 模板。AYIO 出来后，AYShader 单点绕路失去了存在理由。

**教训**：单点避坑（"我这个 TU 不该 include filesystem"）的解药不是写一份手写实现，而是**跟上游模块 owner 沟通修复 namespace 污染**。AYIO 已经把 stat/ofstream 抽象干净，AYShader 没必要再重复一遍。

### 16.9 完成判据

- [x] AYIO `readAllText` / `writeAllText` / `readAllBytes` / `writeAllBytes` / `env::get` / `env::contains` 已实现 + 自测（RFC-011 / RFC-013）
- [x] `CMakeLists.txt` 链 `AYIO`
- [x] 6 个生产 TU 全部迁移，无 `<fstream>` / `<sys/stat.h>` / `<direct.h>` / `GetFileAttributesExA` 残留（grep 验证通过）
- [x] `AYShader_Test` 939 测试不变（migration 是 internal refactor，测试 API 不变）
- [x] design.md §16 + §14.7.4 RFC-011/012/013 已更新
- [x] unittest/ 内 file I/O 不变（按 user 要求）
