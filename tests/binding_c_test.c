/* The kr_binding_* C ABI end to end: a real system and view, values set across reloads, handles that
 * stay the same, types that stay fixed, and view-local handles. Built as karouka_binding_c_test (as
 * C, against the public header only) and run by tools/dev.sh check. */

#include <karouka/karouka.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static double clock_ms;

/* Loads `title`'s page into the view and drives frames until the title shows it was installed. */
static int load_and_settle(kr_view* view, char const* title)
{
    char html[256];
    snprintf(html, sizeof html, "<!doctype html><title>%s</title><body><p id=hp>hp</p></body>", title);
    if (kr_view_load_html(view, html, strlen(html), "file:///karouka-binding-test/") != KR_OK)
        return 0;
    for (int frame = 0; frame < 60; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        if (kr_view_advance_time(view, clock_ms) != KR_OK || kr_view_render(view, &rendered) != KR_OK)
            return 0;
        if (strcmp(kr_view_title(view), title) == 0)
            return 1;
    }
    return 0;
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_binding_c_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    /* Arguments. */
    kr_binding_path untouched = 42;
    CHECK(kr_binding_path_resolve(NULL, "a", &untouched) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_path_resolve(view, NULL, &untouched) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_path_resolve(view, "a", NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_path_resolve(view, "player..health", &untouched) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_path_resolve(view, "", &untouched) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_path_resolve(view, "hud.objective.0", &untouched) == KR_ERROR_INVALID_ARGUMENT); /* no indices */
    CHECK(untouched == 42);
    CHECK(kr_binding_set_number(NULL, 1, 1.0) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_set_number(view, 0, 1.0) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_binding_set_string(view, 1, NULL, 3) == KR_ERROR_INVALID_ARGUMENT);

    /* Resolving before any page is loaded works: the model belongs to the view. */
    kr_binding_path health = 0, name = 0, alive = 0;
    CHECK(kr_binding_path_resolve(view, "player.health", &health) == KR_OK);
    CHECK(kr_binding_path_resolve(view, "player.name", &name) == KR_OK);
    CHECK(kr_binding_path_resolve(view, "player.alive", &alive) == KR_OK);
    CHECK(health != 0 && name != 0 && alive != 0 && health != name && name != alive);
    CHECK(kr_binding_set_number(view, health, 100.0) == KR_OK);

    CHECK(load_and_settle(view, "first"));
    CHECK(kr_binding_set_number(view, health, 82.5) == KR_OK);
    CHECK(kr_binding_set_number(view, health, 82.5) == KR_OK); /* unchanged */
    CHECK(kr_binding_set_string(view, name, "Ada", 3) == KR_OK);
    CHECK(kr_binding_set_string(view, name, "Ada Lovelace (not NUL-terminated)", 12) == KR_OK);
    CHECK(kr_binding_set_bool(view, alive, true) == KR_OK);
    CHECK(kr_binding_set_string(view, name, "\xff", 1) == KR_ERROR_INVALID_ARGUMENT);

    /* Reload twice, with changed writes before and between the reloads. */
    for (int reload = 0; reload < 2; ++reload) {
        char const* title = reload == 0 ? "second" : "first";
        CHECK(load_and_settle(view, title));

        kr_binding_path again = 0;
        CHECK(kr_binding_path_resolve(view, "player.health", &again) == KR_OK && again == health);
        CHECK(kr_binding_path_resolve(view, "player.name", &again) == KR_OK && again == name);
        CHECK(kr_binding_path_resolve(view, "player.alive", &again) == KR_OK && again == alive);

        /* The types fixed before the reload still hold. */
        CHECK(kr_binding_set_bool(view, health, true) == KR_ERROR_INVALID_STATE);
        CHECK(kr_binding_set_number(view, name, 1.0) == KR_ERROR_INVALID_STATE);
        CHECK(kr_binding_set_string(view, alive, "x", 1) == KR_ERROR_INVALID_STATE);

        CHECK(kr_binding_set_number(view, health, 50.0 - reload) == KR_OK);
        CHECK(kr_binding_set_number(view, health, NAN) == KR_OK);
        CHECK(kr_binding_set_string(view, name, reload ? "B" : "A", 1) == KR_OK);
        CHECK(kr_binding_set_bool(view, alive, reload == 0) == KR_OK);

        kr_binding_path fresh = 0;
        CHECK(kr_binding_path_resolve(view, reload ? "hud.objectives.second" : "hud.objectives.first", &fresh) == KR_OK);
        CHECK(fresh > alive);
    }

    /* Handles are view-local: a second view has its own, empty model. */
    kr_view* other = NULL;
    CHECK(kr_view_create2(system, &view_desc, &other) == KR_OK);
    CHECK(kr_binding_set_number(other, health, 1.0) == KR_ERROR_INVALID_ARGUMENT);
    kr_binding_path other_name = 0;
    CHECK(kr_binding_path_resolve(other, "player.name", &other_name) == KR_OK);
    CHECK(other_name == 1);
    CHECK(kr_binding_set_number(other, other_name, 7.0) == KR_OK);
    CHECK(kr_binding_set_number(view, name, 7.0) == KR_ERROR_INVALID_STATE);
    kr_view_destroy(other);

    /* Ceilings through the ABI: path bytes, then the path table. */
    char path[KR_BINDING_MAX_PATH_BYTES + 2];
    memset(path, 'p', KR_BINDING_MAX_PATH_BYTES + 1);
    path[KR_BINDING_MAX_PATH_BYTES + 1] = 0;
    untouched = 42;
    CHECK(kr_binding_path_resolve(view, path, &untouched) == KR_ERROR_LIMIT_EXCEEDED && untouched == 42);
    path[KR_BINDING_MAX_PATH_BYTES] = 0;
    CHECK(kr_binding_path_resolve(view, path, &untouched) == KR_OK);
    int interned = 0;
    for (int i = 0; i < KR_BINDING_MAX_PATHS + 10; ++i) {
        char filler[32];
        snprintf(filler, sizeof filler, "filler.f%d", i);
        kr_binding_path handle = 0;
        kr_result result = kr_binding_path_resolve(view, filler, &handle);
        if (result == KR_ERROR_LIMIT_EXCEEDED)
            break;
        CHECK(result == KR_OK);
        ++interned;
    }
    kr_binding_path again = 0;
    CHECK(kr_binding_path_resolve(view, "player.health", &again) == KR_OK && again == health);
    CHECK(kr_binding_path_resolve(view, "never.seen", &again) == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(interned < KR_BINDING_MAX_PATHS);
    CHECK(kr_binding_path_resolve(view, "filler.f0", &again) == KR_OK);

    /* The big string ceiling. */
    static char big[KR_BINDING_MAX_STRING_BYTES + 1];
    memset(big, 'x', sizeof big);
    CHECK(kr_binding_set_string(view, name, big, KR_BINDING_MAX_STRING_BYTES) == KR_OK);
    CHECK(kr_binding_set_string(view, name, big, KR_BINDING_MAX_STRING_BYTES + 1) == KR_ERROR_LIMIT_EXCEEDED);

    /* One more reload with a full table: the model is still there. */
    CHECK(load_and_settle(view, "third"));
    CHECK(kr_binding_path_resolve(view, "player.alive", &again) == KR_OK && again == alive);
    CHECK(kr_binding_set_bool(view, alive, false) == KR_OK);

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures) {
        fprintf(stderr, "karouka_binding_c_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("karouka_binding_c_test: ok\n");
    return 0;
}
