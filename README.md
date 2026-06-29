# AYShader

> **Phoskia** — φῶς (光) + σκιά (影)，光与影的交织，shader 的本质。

AYShader 是 AY Engine 的着色器子系统：接受 **Phoskia** DSL 源码，经过完整的编译器流水线（**Lex → Parse → AST → 可选 SemA → Backend**），生成目标平台 shader 代码（当前仅 BGFX `.sc`，再交给 shaderc 编译为平台二进制）。

完整设计见 [`design.md`](design.md)。本文档是面向开发者的概览。

---

## 状态

**Phase 2 已关闭**（6 个增量全部完成并通过 golden + e2e 测试）。

| Phase | 范围 | 状态 |
|---|---|---|
| Phase 1 | Lexer + Parser + AST + BGFX 后端 + shaderc e2e | ✅ |
| Phase 2.1 | Variant attribute `#ifdef` opt-in | ✅ |
| Phase 2.2 | TypeInference + SemanticAnalyzer 完整化 + 内置函数库 | ✅ |
| Phase 2.3 | PBR 函数库（Fresnel / GGX / Smith） | ✅ |
| Phase 2.4 | Parser panic-mode 错误恢复 | ✅ |
| Phase 2.5 | Token 降级重构（`vec3`/`float`/... → `Identifier`） | ✅ |
| Phase 2 收尾 | Golden file 验证 + PBR 端到端 demo | ✅ |
| Phase 3 | IR + HLSL/WGSL 多后端 | 🔜 待开始 |

---

## 测试

```bash
# Build（CMake 已配置 AYShader / AYShader_Test）
cmake --build <build-dir>

# 跑全部测试（默认 opt-in：564 测试，包含 shaderc e2e 自动跳过若 shaderc 不可用）
<build-dir>/AYShader_Test.exe

# 重生成 golden baseline（converter 改动后用一次）
AY_SHADER_REGEN_GOLDEN=1 <build-dir>/AYShader_Test.exe
```

最后一次完整跑：**564 / 564 PASS**（截至 commit `ae1028d`）。

### 测试套件

| 文件 | 覆盖 |
|---|---|
| `Test_Lexer.cpp` | Token 类型识别、关键字、数字字面量、字符串、变体属性 |
| `Test_Parser.cpp` | material/property/uniform/texture 声明、shader block、expression |
| `Test_Phoskia.cpp` | 端到端 `Compiler::compile` 通过 |
| `Test_BGFXConverter.cpp` | BGFX 后端输出字符串、three-piece、varying.def.sc |
| `Test_ShaderCompile.cpp` | 端到端 **shaderc** 编译 vs/fs 到 `.bin`，含完整 PBR demo |
| `Test_TypeInference.cpp` | 字面量 / 二元 / swizzle / 索引 / builtin / constructor / unify |
| `Test_SemanticAnalyzer.cpp` | ShaderParam 注册、property 推断、严格 vec4 / bool 检查 |
| `Test_ParserRecovery.cpp` | panic-mode：synchronize() 跳过到 statement boundary |
| `Test_PBRFunctions.cpp` | FresnelSchlick / GGX / Smith 的数学性质 |
| `Test_BuiltinTypes.cpp` | `AYBuiltinTypes::isBuiltinType` 全覆盖 |
| `Test_GoldenFiles.cpp` | 5 个 Phoskia fixture 输出 byte-equal 比对 baseline |

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

---

## 项目结构

```
AYShader/
├── README.md           # 本文件
├── design.md           # 完整设计文档（含类型推导规则、语法 BNF、错误码表）
├── CMakeLists.txt
├── include/
│   ├── AYToken.h
│   ├── AYLexer.h
│   ├── AYParser.h
│   ├── AYAst.h
│   ├── AYType.h
│   ├── AYTypeInference.h
│   ├── AYSemanticAnalyzer.h
│   ├── AYBuiltinTypes.h
│   ├── AYBuiltinFunctions.h
│   ├── AYCompilerError.h
│   ├── AYPhoskia.h
│   ├── IAYBackendConverter.h
│   ├── AYBGFXConverter.h
│   ├── AYShaderProgram.h
│   └── AYShaderCache.h
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