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

**Compute shader 特殊处理**：BGFX 不通过 `.sc` 提供 compute 支持。Phase 2+ Compute shader 走独立路径：
- 生成目标平台的 compute shader 源码（HLSL for DX11/DX12、WebGPU SPIR-V 等）
- 或通过 `bgfx::create_compute_shader()` 直接加载平台特定二进制
- shaderc 工具链需单独调用（`--type compute`）

**长期路线**：

| Shader 类型 | 现状 | Phase 2 | Phase 3 |
|---|---|---|---|
| Vertex | converter 隐式绑定 `return <expr>` → `gl_Position` | 完善 `gl_Position` 语义 | 多后端 |
| Fragment | converter 隐式绑定 `return <expr>` → `gl_FragColor` | 完善 PBR/光照 | 多后端 |
| Compute | BGFX 不支持 `.sc` | DX11/HLSL compute 生成 | SPIR-V / WGSL |
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
<program> ::= <material_decl_list>

<material_decl_list> ::= <material_decl>
                       | <material_decl_list> <material_decl>

<material_decl>     ::= "material" <identifier> "{" <declaration_list> "}"

<declaration_list>   ::= <declaration>
                       | <declaration_list> <declaration>

<declaration>       ::= <property_decl>
                      | <uniform_decl>
                      | <texture_decl>
                      | <sampler_decl>
                      | <vertex_func>
                      | <fragment_func>
                      | <variant_attribute>

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
- [ ] 单元测试与 golden-file 验证
- [ ] **类型名降级重构**（与 type checker 共同推进，详见下文）
- [ ] **Compute shader 后端**（HLSL / SPIR-V 生成路径，BGFX `.sc` 不支持 compute）
- [x] **Shader type 动态输出变量**（`gl_Position` / `gl_FragColor`，已完成 `_shadingOutputVar`）

### Phase 3: IR 与多后端
- [ ] IR 设计实现（SSA 形式）
- [ ] HLSL 后端 (`AYHLSLConverter`) — 含 Compute / Ray shader 支持
- [ ] WGSL 后端 (`AYWGLSConverter`)
- [ ] 跨后端优化（dead code、constant folding）
- [ ] **Ray shader 架构**（独立于 compute 的路径）

### Phase 4: ShaderGraph 与工具链
- [ ] 节点图数据结构
- [ ] 节点 → AST 转换
- [ ] 节点序列化
- [ ] 资源打包与缓存

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
