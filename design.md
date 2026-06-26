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
    shading {
        return color
    }
}
```

PBR：

```phoskia
material PBR {
    texture2d albedoMap
    uniform vec3 cameraPos

    property baseColor = sample(albedoMap, uv)

    shading {
        let N = normalize(worldNormal)
        let L = normalize(lightDir)
        let NdotL = max(dot(N, L), 0.0)
        return vec4(baseColor.rgb * NdotL, 1.0)
    }
}
```

Variant 宏：

```phoskia
material PBR {
    #[variant useEmission]
    property emission = vec3(0.0)

    shading {
        let result = lighting()
        #[variant useEmission]
        result = result + emission
        return result
    }
}
```

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

Phase 1 仅实现 BGFX 后端。未来新增后端：

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
                      | <shading_func>
                      | <vertex_func>
                      | <fragment_func>
                      | <variant_attribute>

<variant_attribute> ::= "[" "variant" <identifier> "]"

<property_decl>     ::= "property" <identifier> "=" <expression> ";"

<uniform_decl>      ::= "uniform" <type> <identifier> ";"

<texture_decl>      ::= "texture2d" <identifier> ";"

<sampler_decl>      ::= "sampler" <identifier> "=" <expression> ";"

<shading_func>      ::= "shading" "{" <statement_list> "}"

<vertex_func>       ::= "vertex" "{" <statement_list> "}"

<fragment_func>     ::= "fragment" "{" <statement_list> "}"

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

### Phase 1: 最小可编译 ✅ 进行中
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
- [ ] 单元测试（端到端编译简单 `.phoskia` → `.sc`）

### Phase 2: 完整 Phoskia 支持
- [ ] 类型推导引擎（`TypeInference` 完整实现）
- [ ] 内置函数库扩充（PBR/光照/纹理采样）
- [ ] Variant 宏在 BGFX 后端的 #ifdef 展开
- [ ] 错误恢复与 panic-mode 验证
- [ ] 单元测试与 golden-file 验证
- [ ] **类型名降级重构**（与 type checker 共同推进，详见下文）

### Phase 3: IR 与多后端
- [ ] IR 设计实现（SSA 形式）
- [ ] HLSL 后端 (`AYHLSLConverter`)
- [ ] WGSL 后端 (`AYWGLSConverter`)
- [ ] 跨后端优化（dead code、constant folding）

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
