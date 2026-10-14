/*
 * The data-bind-* expression language: compiled once, evaluated without the script engine.
 *
 * Tessera's contract: Tessera's data-binding spec § "expression contract"; this is
 * a port of Tessera's implementation onto AK and Karouka::BindingModel.
 *
 * Literals (decimal numbers, true/false, '…'/"…" strings with \\ \' \" \n \r \t escapes), dotted model
 * paths, parentheses, unary + - !, binary * / % + - < <= > >= == !=, and ?: (lazy). Precedence from
 * high to low: parentheses, unary, multiplicative, additive, ordering, equality, ternary.
 *
 * Compilation is a shunting-yard parse into a bounded syntax tree, then an explicit-stack lowering
 * to bytecode: no recursion, fixed ceilings on source bytes, pending operators and instructions.
 * A failed compile leaves no program (evaluate() then fails) and records a diagnostic offset.
 * Evaluation allocates nothing: a fixed operand stack and a 64 KiB string workspace, both owned by
 * the caller. Type errors, unset paths, non-finite numbers and division by zero fail the evaluation
 * with KR_ERROR_INVALID_STATE so the caller leaves its target as it was.
 *
 * Formatters (`name(expr)`) are part of Tessera's contract but none is registered here, so any call
 * syntax is a compile error, as an unknown formatter is there.
 */
#pragma once

#include "BindingModel.h"
#include <AK/ByteString.h>
#include <AK/Vector.h>

namespace Karouka {

struct BindingValue {
    BindingModel::Kind kind { BindingModel::Kind::Unset };
    double number { 0 };
    bool boolean { false };
    StringView string;
};

class BindingExpression {
public:
    static constexpr size_t max_source_bytes = 4096;
    static constexpr size_t max_depth = 64;
    static constexpr size_t max_instructions = 256;
    static constexpr size_t max_string_bytes = 65536;

    struct Limits {
        size_t source_bytes { max_source_bytes };
        size_t depth { max_depth };
        size_t instructions { max_instructions };
    };
    struct Diagnostic {
        kr_result result { KR_OK };
        size_t offset { 0 };
    };
    // Caller-owned, reused across evaluations; one per evaluating thread (here: per system).
    struct Workspace {
        BindingValue stack[max_instructions];
        char strings[max_string_bytes];
        size_t used { 0 };
    };

    kr_result compile(StringView source, BindingModel&);
    kr_result compile(StringView source, BindingModel&, Limits);
    // Borrowed strings in *out point into the program, the model or the workspace.
    kr_result evaluate(BindingModel const&, Workspace&, BindingValue* out) const;

    bool is_compiled() const { return !m_code.is_empty(); }
    bool depends_on(kr_binding_path) const;
    Vector<kr_binding_path> const& dependencies() const { return m_dependencies; }
    Diagnostic diagnostic() const { return m_diagnostic; }

    enum class Op : u8 {
        Number,
        Bool,
        String,
        Path,
        Positive,
        Negative,
        Not,
        Multiply,
        Divide,
        Remainder,
        Add,
        Subtract,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
        Select,
        JumpFalse,
        Jump,
        Open,
        Question,
    };
    struct Instruction {
        Op op { Op::Number };
        u32 offset { 0 };
        u32 size { 0 };
        double number { 0 };
        kr_binding_path path { 0 };
    };

private:
    ByteString m_source;
    Vector<Instruction> m_code;
    Vector<kr_binding_path> m_dependencies;
    Diagnostic m_diagnostic;
};

// Shortest round-trip decimal text of a finite number, locale-independent. Returns the length.
size_t format_binding_number(double, char* buffer, size_t capacity);

}
