/*
 * See BindingExpression.h. Port of Tessera's binding expressions.
 */
#include "BindingExpression.h"
#include <AK/OwnPtr.h>
#include <charconv>
#include <math.h>
#include <string.h>

namespace Karouka {

namespace {

bool is_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_rest(char c) { return is_start(c) || is_digit(c); }

// The data-binding spec's locale-independent decimal policy: 19 significant digits, scale applied in steps of
// at most 22 powers of ten.
double decimal(StringView token)
{
    static constexpr double powers[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    u64 significand = 0;
    int retained = 0, scale = 0;
    size_t i = 0;
    bool fraction = false;
    for (; i < token.length() && token[i] != 'e' && token[i] != 'E'; ++i) {
        char c = token[i];
        if (c == '.') {
            fraction = true;
            continue;
        }
        if (fraction)
            --scale;
        if (!retained && c == '0')
            continue;
        if (retained < 19) {
            significand = significand * 10 + static_cast<u64>(c - '0');
            ++retained;
        } else {
            ++scale;
        }
    }
    if (i < token.length()) {
        ++i;
        bool negative = token[i] == '-';
        if (token[i] == '-' || token[i] == '+')
            ++i;
        int exponent = 0;
        for (; i < token.length(); ++i) {
            if (exponent < 10000)
                exponent = exponent * 10 + (token[i] - '0');
        }
        scale += negative ? -exponent : exponent;
    }
    if (!significand)
        return 0;
    double value = static_cast<double>(significand);
    if (scale > 400)
        return INFINITY;
    if (scale < -400)
        return 0;
    while (scale > 22) {
        value *= 1e22;
        scale -= 22;
    }
    while (scale < -22) {
        value /= 1e22;
        scale += 22;
    }
    return scale < 0 ? value / powers[-scale] : value * powers[scale];
}

using Op = BindingExpression::Op;
using Instruction = BindingExpression::Instruction;

// Parse scratch, never on the C++ stack and never retained by a program. LibWeb runs on one thread,
// so one lazily allocated scratch serves every compile.
struct CompileScratch {
    static constexpr size_t max_nodes = BindingExpression::max_instructions * 2;
    struct Node {
        Instruction instruction;
        size_t children[3] {};
    };
    struct Pending {
        Instruction instruction;
        size_t base { 0 };
        bool is_call { false };
    };
    struct Frame {
        size_t node { 0 }, stage { 0 }, branch { 0 };
    };
    Node nodes[max_nodes];
    size_t values[max_nodes];
    Pending operators[BindingExpression::max_depth];
    Frame frames[max_nodes];
};

CompileScratch& scratch()
{
    static CompileScratch* s_scratch = new CompileScratch;
    return *s_scratch;
}

int precedence(Op op)
{
    switch (op) {
    case Op::Select:
    case Op::Question:
        return 1;
    case Op::Equal:
    case Op::NotEqual:
        return 2;
    case Op::Less:
    case Op::LessEqual:
    case Op::Greater:
    case Op::GreaterEqual:
        return 3;
    case Op::Add:
    case Op::Subtract:
        return 4;
    case Op::Multiply:
    case Op::Divide:
    case Op::Remainder:
        return 5;
    case Op::Positive:
    case Op::Negative:
    case Op::Not:
        return 6;
    default:
        return 0;
    }
}

size_t arity(Op op)
{
    if (op == Op::Select)
        return 3;
    if (op == Op::Positive || op == Op::Negative || op == Op::Not)
        return 1;
    return 2;
}

}

size_t format_binding_number(double value, char* buffer, size_t capacity)
{
    if (value == 0)
        value = 0; // -0 prints as 0
    auto converted = std::to_chars(buffer, buffer + capacity, value);
    if (converted.ec != std::errc {})
        return 0;
    return static_cast<size_t>(converted.ptr - buffer);
}

bool BindingExpression::depends_on(kr_binding_path path) const
{
    for (auto dependency : m_dependencies) {
        if (dependency == path)
            return true;
    }
    return false;
}

kr_result BindingExpression::compile(StringView source, BindingModel& model)
{
    return compile(source, model, Limits {});
}

kr_result BindingExpression::compile(StringView source, BindingModel& model, Limits limits)
{
    m_code.clear();
    m_dependencies.clear();
    m_source = {};
    m_diagnostic = {};
    size_t pos = 0;
    auto error = [&](kr_result result) {
        m_code.clear();
        m_dependencies.clear();
        m_source = {};
        m_diagnostic = { result, pos };
        return result;
    };
    if (source.is_empty())
        return error(KR_ERROR_INVALID_ARGUMENT);
    if (limits.source_bytes > max_source_bytes || limits.depth > max_depth || limits.instructions > max_instructions
        || !limits.depth || !limits.instructions || source.length() > limits.source_bytes)
        return error(KR_ERROR_LIMIT_EXCEEDED);
    m_source = source;
    source = m_source.view();

    auto& s = scratch();
    constexpr size_t max_nodes = CompileScratch::max_nodes;
    auto& nodes = s.nodes;
    auto& values = s.values;
    auto& operators = s.operators;
    auto& frames = s.frames;
    size_t node_count = 0, value_count = 0, operator_count = 0;
    bool operand = true;
    kr_result failure = KR_OK;

    auto push_operator = [&](Instruction instruction, size_t base = 0, bool is_call = false) {
        if (operator_count == limits.depth) {
            failure = KR_ERROR_LIMIT_EXCEEDED;
            return;
        }
        operators[operator_count++] = { instruction, base, is_call };
    };
    auto leaf = [&](Instruction instruction) {
        if (node_count == max_nodes || value_count == max_nodes) {
            failure = KR_ERROR_LIMIT_EXCEEDED;
            return;
        }
        nodes[node_count].instruction = instruction;
        values[value_count++] = node_count++;
    };
    auto reduce = [&]() {
        Instruction instruction = operators[--operator_count].instruction;
        Op op = instruction.op;
        size_t n_args = arity(op);
        if (op == Op::Question || op == Op::Open || value_count < n_args) {
            failure = KR_ERROR_INVALID_ARGUMENT;
            return;
        }
        if (node_count == max_nodes) {
            failure = KR_ERROR_LIMIT_EXCEEDED;
            return;
        }
        auto& node = nodes[node_count];
        node.instruction = instruction;
        for (size_t i = n_args; i > 0; --i)
            node.children[i - 1] = values[--value_count];
        values[value_count++] = node_count++;
    };
    auto whitespace = [&]() {
        while (pos < source.length() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r' || source[pos] == '\n'))
            ++pos;
    };

    while (pos < source.length() && failure == KR_OK) {
        whitespace();
        if (pos == source.length())
            break;
        Instruction token;
        token.offset = static_cast<u32>(pos);
        char c = source[pos];
        if (operand) {
            if (c == '+' || c == '-' || c == '!') {
                token.op = c == '+' ? Op::Positive : c == '-' ? Op::Negative : Op::Not;
                ++pos;
                push_operator(token);
                continue;
            }
            if (c == '(') {
                token.op = Op::Open;
                ++pos;
                push_operator(token, value_count);
                continue;
            }
            if (is_digit(c)) {
                size_t begin = pos++;
                while (pos < source.length() && is_digit(source[pos]))
                    ++pos;
                if (pos < source.length() && source[pos] == '.') {
                    ++pos;
                    while (pos < source.length() && is_digit(source[pos]))
                        ++pos;
                }
                if (pos < source.length() && (source[pos] == 'e' || source[pos] == 'E')) {
                    ++pos;
                    if (pos < source.length() && (source[pos] == '+' || source[pos] == '-'))
                        ++pos;
                    size_t exponent = pos;
                    while (pos < source.length() && is_digit(source[pos]))
                        ++pos;
                    if (exponent == pos)
                        return error(KR_ERROR_INVALID_ARGUMENT);
                }
                token.number = decimal(source.substring_view(begin, pos - begin));
                if (!isfinite(token.number))
                    return error(KR_ERROR_INVALID_ARGUMENT);
                token.op = Op::Number;
                leaf(token);
                operand = false;
                continue;
            }
            if (c == '\'' || c == '"') {
                size_t begin = ++pos;
                while (pos < source.length() && source[pos] != c) {
                    auto byte = static_cast<unsigned char>(source[pos]);
                    if (byte < 32 || byte == 127)
                        return error(KR_ERROR_INVALID_ARGUMENT);
                    if (source[pos++] == '\\') {
                        if (pos == source.length())
                            return error(KR_ERROR_INVALID_ARGUMENT);
                        char escape = source[pos++];
                        if (escape != '\\' && escape != '\'' && escape != '"' && escape != 'n' && escape != 'r' && escape != 't')
                            return error(KR_ERROR_INVALID_ARGUMENT);
                    }
                }
                if (pos == source.length())
                    return error(KR_ERROR_INVALID_ARGUMENT);
                token.op = Op::String;
                token.offset = static_cast<u32>(begin);
                token.size = static_cast<u32>(pos++ - begin);
                leaf(token);
                operand = false;
                continue;
            }
            if (is_start(c)) {
                size_t begin = pos++;
                while (pos < source.length() && (is_rest(source[pos]) || source[pos] == '.'))
                    ++pos;
                auto name = source.substring_view(begin, pos - begin);
                whitespace();
                // No formatter is registered, so call syntax names an unknown formatter.
                if (pos < source.length() && source[pos] == '(')
                    return error(KR_ERROR_INVALID_ARGUMENT);
                if (name == "true"sv || name == "false"sv) {
                    token.op = Op::Bool;
                    token.number = name == "true"sv ? 1 : 0;
                } else {
                    if (name == "null"sv || !BindingModel::path_is_valid(name))
                        return error(KR_ERROR_INVALID_ARGUMENT);
                    token.op = Op::Path;
                    token.size = static_cast<u32>(name.length());
                }
                leaf(token);
                operand = false;
                continue;
            }
            return error(KR_ERROR_INVALID_ARGUMENT);
        }
        if (c == ')') {
            while (operator_count && operators[operator_count - 1].instruction.op != Op::Open && failure == KR_OK)
                reduce();
            if (failure != KR_OK)
                break;
            if (!operator_count)
                return error(KR_ERROR_INVALID_ARGUMENT);
            auto open = operators[--operator_count];
            if (value_count != open.base + 1)
                return error(KR_ERROR_INVALID_ARGUMENT);
            ++pos;
            continue;
        }
        if (c == ':') {
            while (operator_count && operators[operator_count - 1].instruction.op != Op::Question
                && operators[operator_count - 1].instruction.op != Op::Open && failure == KR_OK)
                reduce();
            if (failure != KR_OK)
                break;
            if (!operator_count || operators[operator_count - 1].instruction.op != Op::Question)
                return error(KR_ERROR_INVALID_ARGUMENT);
            operators[operator_count - 1].instruction.op = Op::Select;
            ++pos;
            operand = true;
            continue;
        }
        size_t bytes = 1;
        bool has_equal = pos + 1 < source.length() && source[pos + 1] == '=';
        switch (c) {
        case '+':
            token.op = Op::Add;
            break;
        case '-':
            token.op = Op::Subtract;
            break;
        case '*':
            token.op = Op::Multiply;
            break;
        case '/':
            token.op = Op::Divide;
            break;
        case '%':
            token.op = Op::Remainder;
            break;
        case '<':
            token.op = has_equal ? Op::LessEqual : Op::Less;
            bytes = has_equal ? 2 : 1;
            break;
        case '>':
            token.op = has_equal ? Op::GreaterEqual : Op::Greater;
            bytes = has_equal ? 2 : 1;
            break;
        case '=':
        case '!':
            if (!has_equal)
                return error(KR_ERROR_INVALID_ARGUMENT);
            token.op = c == '=' ? Op::Equal : Op::NotEqual;
            bytes = 2;
            break;
        case '?':
            token.op = Op::Question;
            break;
        default:
            return error(KR_ERROR_INVALID_ARGUMENT);
        }
        // Binary operators associate left; ?: associates right.
        while (operator_count && operators[operator_count - 1].instruction.op != Op::Open
            && (precedence(operators[operator_count - 1].instruction.op) > precedence(token.op)
                || (token.op != Op::Question && precedence(operators[operator_count - 1].instruction.op) == precedence(token.op)))
            && failure == KR_OK)
            reduce();
        if (failure != KR_OK)
            break;
        push_operator(token);
        pos += bytes;
        operand = true;
    }
    if (failure != KR_OK)
        return error(failure);
    if (operand)
        return error(KR_ERROR_INVALID_ARGUMENT);
    while (operator_count && failure == KR_OK)
        reduce();
    if (failure != KR_OK)
        return error(failure);
    if (value_count != 1)
        return error(KR_ERROR_INVALID_ARGUMENT);

    // Lower the tree to bytecode with an explicit stack; ternaries become conditional jumps.
    m_code.ensure_capacity(limits.instructions);
    size_t frame_count = 1;
    frames[0] = { values[0], 0, 0 };
    auto emit = [&](Instruction instruction) {
        if (m_code.size() == limits.instructions) {
            failure = KR_ERROR_LIMIT_EXCEEDED;
            return;
        }
        m_code.append(instruction);
    };
    while (frame_count && failure == KR_OK) {
        auto& frame = frames[frame_count - 1];
        auto& node = nodes[frame.node];
        Op op = node.instruction.op;
        pos = node.instruction.offset;
        if (op == Op::Number || op == Op::Bool || op == Op::String || op == Op::Path) {
            if (op == Op::Path) {
                failure = model.resolve(source.substring_view(node.instruction.offset, node.instruction.size), &node.instruction.path);
                if (failure != KR_OK)
                    break;
                if (!depends_on(node.instruction.path))
                    m_dependencies.append(node.instruction.path);
            }
            emit(node.instruction);
            --frame_count;
            continue;
        }
        if (op == Op::Select && frame.stage == 1) {
            Instruction jump;
            jump.op = Op::JumpFalse;
            frame.branch = m_code.size();
            emit(jump);
        } else if (op == Op::Select && frame.stage == 2) {
            m_code[frame.branch].size = static_cast<u32>(m_code.size() + 1);
            Instruction jump;
            jump.op = Op::Jump;
            frame.branch = m_code.size();
            emit(jump);
        }
        if (failure != KR_OK)
            break;
        if (frame.stage < arity(op)) {
            size_t child = node.children[frame.stage++];
            if (frame_count == max_nodes) {
                failure = KR_ERROR_LIMIT_EXCEEDED;
                break;
            }
            frames[frame_count++] = { child, 0, 0 };
        } else {
            if (op == Op::Select)
                m_code[frame.branch].size = static_cast<u32>(m_code.size());
            else
                emit(node.instruction);
            --frame_count;
        }
    }
    if (failure != KR_OK)
        return error(failure);
    m_code.shrink_to_fit();
    m_dependencies.shrink_to_fit();
    m_diagnostic = {};
    return KR_OK;
}

kr_result BindingExpression::evaluate(BindingModel const& model, Workspace& workspace, BindingValue* out) const
{
    using Kind = BindingModel::Kind;
    if (!out || m_code.is_empty())
        return KR_ERROR_INVALID_STATE;
    workspace.used = 0;
    size_t count = 0;
    auto text = [](BindingValue const& value, char* buffer, size_t capacity, StringView* result) {
        if (value.kind == Kind::String) {
            *result = value.string;
            return true;
        }
        if (value.kind == Kind::Bool) {
            *result = value.boolean ? "true"sv : "false"sv;
            return true;
        }
        size_t length = format_binding_number(value.number, buffer, capacity);
        if (!length)
            return false;
        *result = StringView { buffer, length };
        return true;
    };
    for (size_t pc = 0; pc < m_code.size();) {
        auto const& instruction = m_code[pc++];
        Op op = instruction.op;
        if (op == Op::Jump) {
            pc = instruction.size;
            continue;
        }
        if (op == Op::JumpFalse) {
            if (!count || workspace.stack[count - 1].kind != Kind::Bool)
                return KR_ERROR_INVALID_STATE;
            if (!workspace.stack[--count].boolean)
                pc = instruction.size;
            continue;
        }
        BindingValue value;
        if (op == Op::Number) {
            value.kind = Kind::Number;
            value.number = instruction.number;
        } else if (op == Op::Bool) {
            value.kind = Kind::Bool;
            value.boolean = instruction.number != 0;
        } else if (op == Op::String) {
            value.kind = Kind::String;
            size_t begin = workspace.used;
            for (size_t i = 0; i < instruction.size; ++i) {
                char c = m_source[instruction.offset + i];
                if (c == '\\') {
                    c = m_source[instruction.offset + ++i];
                    if (c == 'n')
                        c = '\n';
                    else if (c == 'r')
                        c = '\r';
                    else if (c == 't')
                        c = '\t';
                }
                if (workspace.used == max_string_bytes)
                    return KR_ERROR_LIMIT_EXCEEDED;
                workspace.strings[workspace.used++] = c;
            }
            value.string = StringView { workspace.strings + begin, workspace.used - begin };
        } else if (op == Op::Path) {
            value.kind = model.kind(instruction.path);
            if (value.kind == Kind::Unset)
                return KR_ERROR_INVALID_STATE;
            if (value.kind == Kind::Number)
                value.number = model.number(instruction.path);
            else if (value.kind == Kind::Bool)
                value.boolean = model.boolean(instruction.path);
            else
                value.string = model.string(instruction.path);
            if (value.kind == Kind::Number && !isfinite(value.number))
                return KR_ERROR_INVALID_STATE;
        } else if (op == Op::Positive || op == Op::Negative || op == Op::Not) {
            if (!count)
                return KR_ERROR_INVALID_STATE;
            value = workspace.stack[--count];
            if (op == Op::Not) {
                if (value.kind != Kind::Bool)
                    return KR_ERROR_INVALID_STATE;
                value.boolean = !value.boolean;
            } else {
                if (value.kind != Kind::Number)
                    return KR_ERROR_INVALID_STATE;
                if (op == Op::Negative)
                    value.number = -value.number;
            }
        } else {
            if (count < 2)
                return KR_ERROR_INVALID_STATE;
            BindingValue right = workspace.stack[--count];
            BindingValue left = workspace.stack[--count];
            if (op == Op::Add && (left.kind == Kind::String || right.kind == Kind::String)) {
                char left_buffer[64], right_buffer[64];
                StringView a, b;
                if (!text(left, left_buffer, sizeof left_buffer, &a) || !text(right, right_buffer, sizeof right_buffer, &b))
                    return KR_ERROR_INVALID_STATE;
                size_t available = max_string_bytes - workspace.used;
                if (a.length() > available || b.length() > available - a.length())
                    return KR_ERROR_LIMIT_EXCEEDED;
                char* dest = workspace.strings + workspace.used;
                // Operands may already live in the workspace below `dest`; memmove keeps that safe.
                if (a.length())
                    memmove(dest, a.characters_without_null_termination(), a.length());
                if (b.length())
                    memmove(dest + a.length(), b.characters_without_null_termination(), b.length());
                workspace.used += a.length() + b.length();
                value.kind = Kind::String;
                value.string = StringView { dest, a.length() + b.length() };
            } else if (op == Op::Equal || op == Op::NotEqual) {
                if (left.kind != right.kind)
                    return KR_ERROR_INVALID_STATE;
                value.kind = Kind::Bool;
                value.boolean = left.kind == Kind::Number ? left.number == right.number
                    : left.kind == Kind::Bool             ? left.boolean == right.boolean
                                                          : left.string == right.string;
                if (op == Op::NotEqual)
                    value.boolean = !value.boolean;
            } else {
                if (left.kind != Kind::Number || right.kind != Kind::Number)
                    return KR_ERROR_INVALID_STATE;
                value.kind = Kind::Number;
                switch (op) {
                case Op::Add:
                    value.number = left.number + right.number;
                    break;
                case Op::Subtract:
                    value.number = left.number - right.number;
                    break;
                case Op::Multiply:
                    value.number = left.number * right.number;
                    break;
                case Op::Divide:
                case Op::Remainder:
                    if (right.number == 0)
                        return KR_ERROR_INVALID_STATE;
                    value.number = op == Op::Divide ? left.number / right.number : fmod(left.number, right.number);
                    break;
                case Op::Less:
                    value.kind = Kind::Bool;
                    value.boolean = left.number < right.number;
                    break;
                case Op::LessEqual:
                    value.kind = Kind::Bool;
                    value.boolean = left.number <= right.number;
                    break;
                case Op::Greater:
                    value.kind = Kind::Bool;
                    value.boolean = left.number > right.number;
                    break;
                case Op::GreaterEqual:
                    value.kind = Kind::Bool;
                    value.boolean = left.number >= right.number;
                    break;
                default:
                    return KR_ERROR_INVALID_STATE;
                }
                if (value.kind == Kind::Number && !isfinite(value.number))
                    return KR_ERROR_INVALID_STATE;
            }
        }
        if (count == max_instructions)
            return KR_ERROR_LIMIT_EXCEEDED;
        workspace.stack[count++] = value;
    }
    if (count != 1)
        return KR_ERROR_INVALID_STATE;
    *out = workspace.stack[0];
    return KR_OK;
}

}
