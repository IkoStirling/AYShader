#pragma once
// AYAst.h - AST node definitions for Phoskia
//
// Class layout (MSVC requires this order to avoid incomplete-type errors):
//   1) forward declarations of every class
//   2) forward declarations of every node's class
//   3) full definition of AstNode (base class with virtual accept())
//   4) full definitions of all concrete node classes (Expr / Stmt / decls)
//      — accept() is DECLARED inside the class, defined OUT-OF-LINE
//        after AstVisitor is complete
//   5) full definition of AstVisitor
//   6) out-of-line definitions of all accept() methods

#include "AYToken.h"
#include <array>
#include <memory>
#include <vector>
#include <string>
#include <variant>

namespace ayt::shader::phoskia
{

// (1) Forward declarations of all classes referenced as types
class AstNode;
class AstVisitor;
class Expr;
class Stmt;
class Program;
class BinaryExpr;
class UnaryExpr;
class CallExpr;
class IdentifierExpr;
class LiteralExpr;
class MemberExpr;
class IndexExpr;
class LetStmt;
class ReturnStmt;
class IfStmt;
class ForStmt;
class ExprStmt;
class MaterialDecl;
class PropertyDecl;
class UniformDecl;
class TextureDecl;
class StorageDecl;
class VertexFunc;
class FragmentFunc;
class ComputeDecl;
class ShaderParam;
class VariantAttribute;

// Phoskia semantic types (used by ShaderParam) — replaces bgfx
// POSITION/NORMAL/COLOR0/TEXCOORD0 from the programmer's perspective.
// Mapping to bgfx semantic slots lives in the converter.
enum class PhoskiaSemantic : uint8_t {
    Position,
    Normal,
    Color,
    Texcoord,
};

// (2) AstNode base — must come BEFORE any class derives from it.
class AstNode {
public:
    virtual ~AstNode() = default;
    virtual void accept(AstVisitor& visitor) = 0;
};

// (3) Concrete node classes — all derive from AstNode.
//     `accept()` is declared (with `override`) but defined out-of-line,
//     AFTER AstVisitor is fully defined. This sidesteps the
//     "incomplete-type-for-virtual-override" error from MSVC.
class Expr : public AstNode {
public:
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

class BinaryExpr : public Expr {
public:
    BinaryExpr(ExprPtr left, Token op, ExprPtr right)
        : left(std::move(left)), op(op), right(std::move(right)) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr left;
    Token op;
    ExprPtr right;
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(Token op, ExprPtr operand)
        : op(op), operand(std::move(operand)) {}
    void accept(AstVisitor& visitor) override;
    Token op;
    ExprPtr operand;
};

class CallExpr : public Expr {
public:
    CallExpr(ExprPtr callee, std::vector<ExprPtr> args)
        : callee(std::move(callee)), args(std::move(args)) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(const std::string& name) : name(name) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
};

class LiteralExpr : public Expr {
public:
    LiteralExpr(std::variant<std::monostate, bool, float, int, std::string> value)
        : value(value) {}
    void accept(AstVisitor& visitor) override;
    std::variant<std::monostate, bool, float, int, std::string> value;
};

class MemberExpr : public Expr {
public:
    MemberExpr(ExprPtr object, const std::string& member)
        : object(std::move(object)), member(member) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr object;
    std::string member;
};

class IndexExpr : public Expr {
public:
    IndexExpr(ExprPtr object, ExprPtr index)
        : object(std::move(object)), index(std::move(index)) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr object;
    ExprPtr index;
};

class Stmt : public AstNode {
public:
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

class LetStmt : public Stmt {
public:
    LetStmt(const std::string& name, ExprPtr initializer)
        : name(name), initializer(std::move(initializer)) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
    ExprPtr initializer;
};

class ReturnStmt : public Stmt {
public:
    explicit ReturnStmt(ExprPtr value) : value(std::move(value)) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr value;
};

class IfStmt : public Stmt {
public:
    IfStmt(ExprPtr condition, std::vector<StmtPtr> thenBranch, std::vector<StmtPtr> elseBranch)
        : condition(std::move(condition)),
          thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch)) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr condition;
    std::vector<StmtPtr> thenBranch;
    std::vector<StmtPtr> elseBranch;
};

class ForStmt : public Stmt {
public:
    ForStmt(const std::string& variable, ExprPtr iterable, std::vector<StmtPtr> body)
        : variable(variable), iterable(std::move(iterable)), body(std::move(body)) {}
    void accept(AstVisitor& visitor) override;
    std::string variable;
    ExprPtr iterable;
    std::vector<StmtPtr> body;
};

class ExprStmt : public Stmt {
public:
    explicit ExprStmt(ExprPtr expr) : expr(std::move(expr)) {}
    void accept(AstVisitor& visitor) override;
    ExprPtr expr;
};

class MaterialDecl : public Stmt {
public:
    MaterialDecl(const std::string& name, std::vector<StmtPtr> declarations)
        : name(name), declarations(std::move(declarations)) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
    std::vector<StmtPtr> declarations;
};

class PropertyDecl : public Stmt {
public:
    PropertyDecl(const std::string& name, ExprPtr initializer)
        : name(name), initializer(std::move(initializer)) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
    ExprPtr initializer;
};

class UniformDecl : public Stmt {
public:
    UniformDecl(const std::string& type, const std::string& name)
        : type(type), name(name) {}
    void accept(AstVisitor& visitor) override;
    std::string type;
    std::string name;
};

class TextureDecl : public Stmt {
public:
    explicit TextureDecl(const std::string& name) : name(name) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
};

// Phase 3.2 Block 3: compute storage buffer declaration.
//   storage <name> : structuredbuffer<T>     — read access
//   storage <name> : rwstructuredbuffer<T>   — read-write access
//
// Element type (`T`) is captured as a GLSL lexeme string in Phase 3.2 —
// the parser only accepts builtin scalar / vector types (float, int,
// vec2..4, ivec2..4). Custom struct types are Phase 3.3 work.
//
// The kind (structuredbuffer vs rwstructuredbuffer) is the access mode.
// In GLSL both forms lower to the same `buffer Name { T data[]; } Name;`
// syntax (GLSL doesn't distinguish read-only storage buffers at the
// source level — that's a HLSL-only thing), but the access field is
// preserved in the IR for future HLSL emitter use.
class StorageDecl : public Stmt {
public:
    enum class Access { Read, ReadWrite };
    StorageDecl(Access access, const std::string& name, const std::string& elementType)
        : access(access), name(name), elementType(elementType) {}
    void accept(AstVisitor& visitor) override;
    Access access;
    std::string name;
    std::string elementType;
};

// Parameter declaration inside vertex { } or fragment { }.
// `in`  → input attribute (vertex) or input varying (fragment)
// `out` → output varying (vertex only; fragment has no out)
class ShaderParam : public Stmt {
public:
    enum class Direction { In, Out };
    ShaderParam(Direction dir, const std::string& name, PhoskiaSemantic semantic,
                ExprPtr defaultValue)
        : dir(dir), name(name), semantic(semantic),
          defaultValue(std::move(defaultValue)) {}
    void accept(AstVisitor& visitor) override;
    Direction dir;
    std::string name;
    PhoskiaSemantic semantic;
    // Only meaningful for `out` params; ignored for `in`.
    ExprPtr defaultValue;
};

class VertexFunc : public Stmt {
public:
    VertexFunc(std::vector<StmtPtr> params, std::vector<StmtPtr> body)
        : params(std::move(params)), body(std::move(body)) {}
    void accept(AstVisitor& visitor) override;
    // In / Out param declarations in declaration order.
    std::vector<StmtPtr> params;
    std::vector<StmtPtr> body;
};

class FragmentFunc : public Stmt {
public:
    FragmentFunc(std::vector<StmtPtr> inputs, std::vector<StmtPtr> body)
        : inputs(std::move(inputs)), body(std::move(body)) {}
    void accept(AstVisitor& visitor) override;
    // Only `in` params — fragments have no outputs.
    std::vector<StmtPtr> inputs;
    std::vector<StmtPtr> body;
};

// Top-level GPGPU compute kernel declaration. Phase 2.5 introduces
// this in the parser / AST and surfaces a friendly diagnostic from
// the BGFX backend ("HLSL / WGSL backend required (Phase 3)"). The
// actual HLSL / WGSL backend that lowers this to platform-native
// compute shader source is Phase 3 work.
//
// Compute is *not* a Material: there is no implicit output slot, no
// in/out semantic binding, no gl_Position / gl_FragColor analogue.
// `return` in a compute body is early-exit (the thread has nothing
// to do) — it does NOT bind a return value to a fixed output. There
// is no `params` / `inputs` vector because storage buffers and
// thread-id builtins will be Phase 3 additions alongside the HLSL
// backend (`storage T : structuredbuffer` declarations and
// `thread_id` / `group_id` builtins live in the body).
class ComputeDecl : public Stmt {
public:
    ComputeDecl(const std::string& name, std::vector<StmtPtr> body)
        : name(name), body(std::move(body)) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
    std::vector<StmtPtr> body;
    // Phase 3.3 Block 2: optional [numthreads(X, Y, Z)] attribute.
    // hasNumThreads distinguishes "user wrote [numthreads(8, 8, 1)]"
    // (true) from "user didn't specify" (false — BGFX backend falls
    // back to its hardcoded 64 default). When false, numThreads is
    // uninitialised / leftover; check hasNumThreads before reading.
    bool hasNumThreads = false;
    std::array<uint32_t, 3> numThreads{};
};

class VariantAttribute : public Stmt {
public:
    explicit VariantAttribute(const std::string& name) : name(name) {}
    void accept(AstVisitor& visitor) override;
    std::string name;
};

class Program : public AstNode {
public:
    Program(std::vector<StmtPtr> declarations) : declarations(std::move(declarations)) {}
    void accept(AstVisitor& visitor) override;
    std::vector<StmtPtr> declarations;
};

// (4) AstVisitor — defined AFTER all parameter types are complete
class AstVisitor {
public:
    virtual ~AstVisitor() = default;

    virtual void visit(Program& node) = 0;
    virtual void visit(MaterialDecl& node) = 0;
    virtual void visit(PropertyDecl& node) = 0;
    virtual void visit(UniformDecl& node) = 0;
    virtual void visit(TextureDecl& node) = 0;
    virtual void visit(StorageDecl& node) = 0;
    virtual void visit(ShaderParam& node) = 0;
    virtual void visit(VertexFunc& node) = 0;
    virtual void visit(FragmentFunc& node) = 0;
    virtual void visit(ComputeDecl& node) = 0;

    virtual void visit(BinaryExpr& node) = 0;
    virtual void visit(UnaryExpr& node) = 0;
    virtual void visit(CallExpr& node) = 0;
    virtual void visit(IdentifierExpr& node) = 0;
    virtual void visit(LiteralExpr& node) = 0;
    virtual void visit(MemberExpr& node) = 0;
    virtual void visit(IndexExpr& node) = 0;

    virtual void visit(LetStmt& node) = 0;
    virtual void visit(ReturnStmt& node) = 0;
    virtual void visit(IfStmt& node) = 0;
    virtual void visit(ForStmt& node) = 0;
    virtual void visit(ExprStmt& node) = 0;
    virtual void visit(VariantAttribute& node) = 0;
};

// (5) accept() out-of-line definitions — now that AstVisitor is complete,
//     these just forward to the corresponding visit() overload.
inline void BinaryExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void UnaryExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void CallExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void IdentifierExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void LiteralExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void MemberExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void IndexExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void LetStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void ReturnStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void IfStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void ForStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void ExprStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void MaterialDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void PropertyDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void UniformDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void TextureDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void StorageDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void ShaderParam::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void VertexFunc::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void FragmentFunc::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void ComputeDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void VariantAttribute::accept(AstVisitor& visitor) { visitor.visit(*this); }
inline void Program::accept(AstVisitor& visitor) { visitor.visit(*this); }

} // namespace ayt::shader::phoskia
