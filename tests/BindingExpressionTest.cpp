// Unit tests of Karouka::BindingExpression (src/engine/BindingExpression.h), the expression
// contract of Tessera's data-binding spec. Built as karouka_binding_expression_test and
// run by tools/dev.sh check.

#include "BindingExpression.h"

#include <AK/Format.h>
#include <AK/StringBuilder.h>

using Karouka::BindingExpression;
using Karouka::BindingModel;
using Karouka::BindingValue;
using Kind = BindingModel::Kind;

static int s_failures;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            warnln("{}:{}: CHECK failed: {}", __FILE__, __LINE__, #expr); \
            ++s_failures;                                                 \
        }                                                                 \
    } while (0)

static BindingExpression::Workspace* s_workspace = new BindingExpression::Workspace;

static kr_result eval(BindingModel& model, StringView source, BindingValue* out)
{
    BindingExpression expression;
    if (auto result = expression.compile(source, model); result != KR_OK)
        return result;
    return expression.evaluate(model, *s_workspace, out);
}

static bool is_number(BindingModel& model, StringView source, double expected)
{
    BindingValue value;
    return eval(model, source, &value) == KR_OK && value.kind == Kind::Number && value.number == expected;
}

static bool is_bool(BindingModel& model, StringView source, bool expected)
{
    BindingValue value;
    return eval(model, source, &value) == KR_OK && value.kind == Kind::Bool && value.boolean == expected;
}

static bool is_string(BindingModel& model, StringView source, StringView expected)
{
    BindingValue value;
    return eval(model, source, &value) == KR_OK && value.kind == Kind::String && value.string == expected;
}

static void test_operators()
{
    BindingModel model;
    CHECK(is_number(model, "1 + 2 * 3"sv, 7));
    CHECK(is_number(model, "(1 + 2) * 3"sv, 9));
    CHECK(is_number(model, "10 - 4 - 3"sv, 3));
    CHECK(is_number(model, "7 % 4"sv, 3));
    CHECK(is_number(model, "8 / 2 / 2"sv, 2));
    CHECK(is_number(model, "-2 * -3"sv, 6));
    CHECK(is_number(model, "+5"sv, 5));
    CHECK(is_number(model, "1.5e2"sv, 150));
    CHECK(is_number(model, "1 + 2 * 3 == 7 ? 10 : 20"sv, 10));
    CHECK(is_number(model, "false ? 1 : true ? 2 : 3"sv, 2));
    CHECK(is_bool(model, "1 < 2"sv, true));
    CHECK(is_bool(model, "2 <= 2"sv, true));
    CHECK(is_bool(model, "1 > 2"sv, false));
    CHECK(is_bool(model, "2 >= 3"sv, false));
    CHECK(is_bool(model, "!(1 != 1)"sv, true));
    CHECK(is_bool(model, "-0 == 0"sv, true));
    CHECK(is_bool(model, "'a' == \"a\""sv, true));
    CHECK(is_string(model, "'hp ' + 82.5"sv, "hp 82.5"sv));
    CHECK(is_string(model, "1 + 2 + 'x'"sv, "3x"sv));
    CHECK(is_string(model, "'x' + 1 + 2"sv, "x12"sv));
    CHECK(is_string(model, "true + '!'"sv, "true!"sv));
    CHECK(is_string(model, "'a\\'b\\n'"sv, "a'b\n"sv));

    BindingValue value;
    CHECK(eval(model, "1 / 0"sv, &value) == KR_ERROR_INVALID_STATE);
    CHECK(eval(model, "1 % 0"sv, &value) == KR_ERROR_INVALID_STATE);
    CHECK(eval(model, "1 + true"sv, &value) == KR_ERROR_INVALID_STATE);
    CHECK(eval(model, "1 == '1'"sv, &value) == KR_ERROR_INVALID_STATE);
    CHECK(eval(model, "!1"sv, &value) == KR_ERROR_INVALID_STATE);
    CHECK(eval(model, "1 ? 2 : 3"sv, &value) == KR_ERROR_INVALID_STATE);
    CHECK(eval(model, "1e308 * 10"sv, &value) == KR_ERROR_INVALID_STATE);
}

static void test_syntax_errors()
{
    BindingModel model;
    BindingValue value;
    for (auto source : { ""sv, "1 +"sv, ".5"sv, "0x10"sv, "null"sv, "a = 1"sv, "a[0]"sv, "a && b"sv, "f(1)"sv,
             "(1"sv, "1)"sv, "1 ? 2"sv, "'open"sv, "'\\q'"sv, "1e"sv, "a..b"sv, "// c"sv, "1 2"sv }) {
        BindingExpression expression;
        auto result = expression.compile(source, model);
        if (result != KR_ERROR_INVALID_ARGUMENT)
            warnln("  source '{}' gave {}", source, static_cast<int>(result));
        CHECK(result == KR_ERROR_INVALID_ARGUMENT);
        CHECK(!expression.is_compiled());
        CHECK(expression.evaluate(model, *s_workspace, &value) == KR_ERROR_INVALID_STATE);
    }
    BindingExpression expression;
    CHECK(expression.compile("1 + + +"sv, model) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(expression.diagnostic().result == KR_ERROR_INVALID_ARGUMENT);
    // true.value is a path, not the literal.
    CHECK(expression.compile("true.value"sv, model) == KR_OK);
}

static void test_paths()
{
    BindingModel model;
    BindingExpression expression;
    CHECK(expression.compile("player.alive ? 'hp ' + player.health : enemy.name"sv, model) == KR_OK);
    CHECK(expression.dependencies().size() == 3);
    kr_binding_path health = 0, alive = 0, name = 0, other = 0;
    CHECK(model.resolve("player.health"sv, &health) == KR_OK && expression.depends_on(health));
    CHECK(model.resolve("player.alive"sv, &alive) == KR_OK && expression.depends_on(alive));
    CHECK(model.resolve("enemy.name"sv, &name) == KR_OK && expression.depends_on(name));
    CHECK(model.resolve("other"sv, &other) == KR_OK && !expression.depends_on(other));

    BindingValue value;
    // Unset selected path: error, so the target stays authored.
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_ERROR_INVALID_STATE);
    CHECK(model.set_bool(alive, true) == KR_OK);
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_ERROR_INVALID_STATE);
    CHECK(model.set_number(health, 5) == KR_OK);
    CHECK(model.set_number(health, 82.5) == KR_OK); // coalesced: the last value is what evaluates
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_OK && value.string == "hp 82.5"sv);
    // The unselected arm (enemy.name, unset) does not prevent a result; now select it.
    CHECK(model.set_bool(alive, false) == KR_OK);
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_ERROR_INVALID_STATE);
    char caller[] = "Ada";
    CHECK(model.set_string(name, StringView { caller, 3 }) == KR_OK);
    caller[0] = 'X';
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_OK && value.string == "Ada"sv);
}

static void test_limits()
{
    BindingModel model;
    BindingExpression expression;
    // Source ceiling: exactly at the limit is fine, one byte beyond is not.
    StringBuilder source;
    source.append('1');
    while (source.length() < BindingExpression::max_source_bytes)
        source.append(' ');
    CHECK(expression.compile(source.string_view(), model) == KR_OK);
    source.append(' ');
    CHECK(expression.compile(source.string_view(), model) == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(!expression.is_compiled()); // failed recompile disables the old program

    // Nesting: 64 pending parentheses fit, 65 do not; 10000 is bounded, not a stack overflow.
    auto nested = [](size_t depth, char open, StringView inner) {
        StringBuilder builder;
        for (size_t i = 0; i < depth; ++i)
            builder.append(open);
        builder.append(inner);
        if (open == '(') {
            for (size_t i = 0; i < depth; ++i)
                builder.append(')');
        }
        return builder.to_byte_string();
    };
    CHECK(expression.compile(nested(64, '(', "1"sv), model) == KR_OK);
    CHECK(expression.compile(nested(65, '(', "1"sv), model) == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(expression.diagnostic().result == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(expression.compile(nested(1000, '(', "1"sv), model) == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(expression.compile(nested(64, '-', "1"sv), model) == KR_OK);
    CHECK(expression.compile(nested(4000, '!', "true"sv), model) == KR_ERROR_LIMIT_EXCEEDED);
    StringBuilder ternary;
    for (int i = 0; i < 100; ++i)
        ternary.append("true ? 1 : "sv);
    ternary.append('0');
    CHECK(expression.compile(ternary.string_view(), model) == KR_ERROR_LIMIT_EXCEEDED);

    // Instructions: 1+1+...+1 with 128 ones is 255 instructions; 129 ones is 257.
    auto sum = [](size_t ones) {
        StringBuilder builder;
        builder.append('1');
        for (size_t i = 1; i < ones; ++i)
            builder.append("+1"sv);
        return builder.to_byte_string();
    };
    CHECK(expression.compile(sum(128), model) == KR_OK);
    BindingValue value;
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_OK && value.number == 128);
    CHECK(expression.compile(sum(129), model) == KR_ERROR_LIMIT_EXCEEDED);
    BindingExpression::Limits limits;
    limits.instructions = 3;
    CHECK(expression.compile("1+1"sv, model, limits) == KR_OK);
    CHECK(expression.compile("1+1+1"sv, model, limits) == KR_ERROR_LIMIT_EXCEEDED);

    // Intermediate strings share one 64 KiB workspace: exhaustion fails without a partial result.
    kr_binding_path big = 0;
    CHECK(model.resolve("big"sv, &big) == KR_OK);
    StringBuilder text;
    for (size_t i = 0; i < BindingModel::max_string_bytes; ++i)
        text.append('x');
    CHECK(model.set_string(big, text.string_view()) == KR_OK);
    CHECK(expression.compile("big + ''"sv, model) == KR_OK);
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_OK && value.string.length() == BindingModel::max_string_bytes);
    CHECK(expression.compile("big + 'y'"sv, model) == KR_OK);
    CHECK(expression.evaluate(model, *s_workspace, &value) == KR_ERROR_LIMIT_EXCEEDED);
}

int main()
{
    test_operators();
    test_syntax_errors();
    test_paths();
    test_limits();
    if (s_failures) {
        warnln("karouka_binding_expression_test: {} check(s) failed", s_failures);
        return 1;
    }
    outln("karouka_binding_expression_test: ok");
    return 0;
}
