// Unit tests of Karouka::BindingModel (src/engine/BindingModel.h): resolve, setters, coalescing,
// ceilings. Built as karouka_binding_model_test and run by tools/dev.sh check.

#include "BindingModel.h"

#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <math.h>

using Karouka::BindingModel;
using Kind = BindingModel::Kind;

static int s_failures;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            warnln("{}:{}: CHECK failed: {}", __FILE__, __LINE__, #expr); \
            ++s_failures;                                                 \
        }                                                                 \
    } while (0)

static kr_binding_path resolve(BindingModel& model, StringView path)
{
    kr_binding_path handle = 0;
    auto result = model.resolve(path, &handle);
    if (result != KR_OK) {
        warnln("resolve('{}') failed: {}", path, static_cast<int>(result));
        ++s_failures;
    }
    return handle;
}

static void test_path_grammar()
{
    for (auto path : { "a"sv, "_"sv, "player.health"sv, "A_b9.c_D.e1"sv, "x.y.z.w"sv, "_0._1"sv })
        CHECK(BindingModel::path_is_valid(path));
    for (auto path : { ""sv, "."sv, "a."sv, ".a"sv, "a..b"sv, "9a"sv, "a.9"sv, "a b"sv, "a-b"sv, "a[0]"sv, "a.b "sv, "é"sv, "a\0b"sv })
        CHECK(!BindingModel::path_is_valid(path));

    BindingModel model;
    kr_binding_path out = 1234;
    CHECK(model.resolve("a..b"sv, &out) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(model.resolve(""sv, &out) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(out == 1234);
    CHECK(model.path_count() == 0);
    CHECK(model.resolve("a"sv, nullptr) == KR_ERROR_INVALID_ARGUMENT);
}

static void test_resolve_interns()
{
    BindingModel model;
    auto health = resolve(model, "player.health"sv);
    auto name = resolve(model, "player.name"sv);
    CHECK(health != 0);
    CHECK(name != 0);
    CHECK(health != name);
    CHECK(resolve(model, "player.health"sv) == health);
    CHECK(resolve(model, "Player.health"sv) != health); // case-sensitive
    CHECK(model.path_count() == 3);
    CHECK(model.path(health) == "player.health"sv);
    CHECK(model.kind(health) == Kind::Unset);
    CHECK(!model.is_dirty(health));
    CHECK(!model.has_dirty());
    CHECK(!model.is_valid(0));
    CHECK(!model.is_valid(4));
}

static void test_setters_and_types()
{
    BindingModel model;
    auto n = resolve(model, "n"sv);
    auto b = resolve(model, "b"sv);
    auto s = resolve(model, "s"sv);

    CHECK(model.set_number(n, 82.5) == KR_OK);
    CHECK(model.kind(n) == Kind::Number);
    CHECK(model.number(n) == 82.5);
    CHECK(model.set_bool(b, true) == KR_OK);
    CHECK(model.kind(b) == Kind::Bool && model.boolean(b));
    CHECK(model.set_string(s, "héllo"sv) == KR_OK);
    CHECK(model.kind(s) == Kind::String && model.string(s) == "héllo"sv);

    // The first set fixes the type: other setters fail and change nothing.
    model.take_dirty();
    CHECK(model.set_bool(n, true) == KR_ERROR_INVALID_STATE);
    CHECK(model.set_string(n, "1"sv) == KR_ERROR_INVALID_STATE);
    CHECK(model.set_number(b, 1) == KR_ERROR_INVALID_STATE);
    CHECK(model.set_number(s, 1) == KR_ERROR_INVALID_STATE);
    CHECK(model.kind(n) == Kind::Number && model.number(n) == 82.5);
    CHECK(model.kind(b) == Kind::Bool && model.boolean(b));
    CHECK(model.kind(s) == Kind::String && model.string(s) == "héllo"sv);
    CHECK(!model.has_dirty());

    // Unknown handles.
    CHECK(model.set_number(0, 1) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(model.set_bool(99, true) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(model.set_string(4, "x"sv) == KR_ERROR_INVALID_ARGUMENT);

    // Invalid UTF-8 is refused and keeps the old value; an empty string is a value.
    CHECK(model.set_string(s, "\xff\xfe"sv) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(model.string(s) == "héllo"sv);
    CHECK(model.set_string(s, ""sv) == KR_OK);
    CHECK(model.string(s).is_empty() && model.kind(s) == Kind::String);

    // A failed first set leaves the slot unset, so another type can still claim it.
    auto u = resolve(model, "u"sv);
    CHECK(model.set_string(u, "\xc3"sv) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(model.kind(u) == Kind::Unset && !model.is_dirty(u));
    CHECK(model.set_bool(u, false) == KR_OK);
    CHECK(model.kind(u) == Kind::Bool && model.is_dirty(u)); // unset -> false is a change
}

static void test_dirty_and_coalescing()
{
    BindingModel model;
    auto a = resolve(model, "a"sv);
    auto b = resolve(model, "b"sv);
    auto c = resolve(model, "c"sv);

    CHECK(model.set_number(b, 1) == KR_OK);
    CHECK(model.set_number(a, 1) == KR_OK);
    CHECK(model.set_number(b, 2) == KR_OK);
    CHECK(model.set_number(b, 3) == KR_OK);
    // b, then a: first-change order, each once, holding the last value.
    CHECK(model.dirty().size() == 2);
    CHECK(model.dirty()[0] == b && model.dirty()[1] == a);
    CHECK(model.number(b) == 3);
    CHECK(!model.is_dirty(c));

    auto batch = model.take_dirty();
    CHECK(batch.size() == 2);
    CHECK(!model.has_dirty() && !model.is_dirty(a) && !model.is_dirty(b));

    // Unchanged writes mark nothing.
    CHECK(model.set_number(a, 1) == KR_OK);
    CHECK(model.set_number(b, 3) == KR_OK);
    CHECK(model.set_bool(c, true) == KR_OK);
    model.take_dirty();
    CHECK(model.set_bool(c, true) == KR_OK);
    CHECK(!model.has_dirty());

    // Numbers compare bitwise: NaN twice is unchanged, -0 after +0 is a change.
    CHECK(model.set_number(a, NAN) == KR_OK);
    model.take_dirty();
    CHECK(model.set_number(a, NAN) == KR_OK);
    CHECK(!model.has_dirty());
    CHECK(model.set_number(a, 0.0) == KR_OK);
    model.take_dirty();
    CHECK(model.set_number(a, -0.0) == KR_OK);
    CHECK(model.is_dirty(a));
    model.take_dirty();

    // A change and its revert within one batch still leaves the slot queued once.
    auto s = resolve(model, "s"sv);
    CHECK(model.set_string(s, "x"sv) == KR_OK);
    model.take_dirty();
    CHECK(model.set_string(s, "y"sv) == KR_OK);
    CHECK(model.set_string(s, "x"sv) == KR_OK);
    CHECK(model.dirty().size() == 1 && model.string(s) == "x"sv);
    model.take_dirty();
    CHECK(model.set_string(s, "x"sv) == KR_OK);
    CHECK(!model.has_dirty());
}

static void test_ceilings()
{
    // Path bytes: the ceiling is inclusive.
    {
        BindingModel model;
        StringBuilder longest;
        longest.append_repeated('p', BindingModel::max_path_bytes);
        kr_binding_path out = 0;
        CHECK(model.resolve(longest.string_view(), &out) == KR_OK && out != 0);
        longest.append('p');
        kr_binding_path untouched = 77;
        CHECK(model.resolve(longest.string_view(), &untouched) == KR_ERROR_LIMIT_EXCEEDED);
        CHECK(untouched == 77);
        CHECK(model.path_count() == 1);
    }

    // Path count: max_paths interned, the next new one refused, existing ones still resolve.
    {
        BindingModel model;
        kr_binding_path first = 0, last = 0;
        for (size_t i = 0; i < BindingModel::max_paths; ++i) {
            auto path = ByteString::formatted("slot{}", i);
            kr_binding_path handle = 0;
            if (model.resolve(path, &handle) != KR_OK) {
                warnln("resolve of path {} failed", i);
                ++s_failures;
                break;
            }
            if (i == 0)
                first = handle;
            last = handle;
        }
        CHECK(model.path_count() == BindingModel::max_paths);
        CHECK(last == BindingModel::max_paths);
        kr_binding_path out = 55;
        CHECK(model.resolve("one.more"sv, &out) == KR_ERROR_LIMIT_EXCEEDED);
        CHECK(out == 55);
        CHECK(model.path_count() == BindingModel::max_paths);
        CHECK(model.resolve("slot0"sv, &out) == KR_OK && out == first);
        CHECK(model.resolve("slot4095"sv, &out) == KR_OK && out == last);
        // Every slot dirty at once: the dirty list is bounded by the path table.
        for (kr_binding_path h = 1; h <= BindingModel::max_paths; ++h)
            CHECK(model.set_number(h, h) == KR_OK);
        CHECK(model.dirty().size() == BindingModel::max_paths);
    }

    // String bytes: inclusive, and a refused string keeps the previous value.
    {
        BindingModel model;
        auto s = resolve(model, "s"sv);
        StringBuilder largest;
        largest.append_repeated('x', BindingModel::max_string_bytes);
        CHECK(model.set_string(s, largest.string_view()) == KR_OK);
        CHECK(model.string(s).length() == BindingModel::max_string_bytes);
        model.take_dirty();
        largest.append('x');
        CHECK(model.set_string(s, largest.string_view()) == KR_ERROR_LIMIT_EXCEEDED);
        CHECK(model.string(s).length() == BindingModel::max_string_bytes);
        CHECK(!model.has_dirty());
        // A refused first set leaves an unset slot unset.
        auto t = resolve(model, "t"sv);
        CHECK(model.set_string(t, largest.string_view()) == KR_ERROR_LIMIT_EXCEEDED);
        CHECK(model.kind(t) == Kind::Unset);
    }
}

int main()
{
    test_path_grammar();
    test_resolve_interns();
    test_setters_and_types();
    test_dirty_and_coalescing();
    test_ceilings();
    if (s_failures) {
        warnln("karouka_binding_model_test: {} check(s) failed", s_failures);
        return 1;
    }
    outln("karouka_binding_model_test: ok");
    return 0;
}
