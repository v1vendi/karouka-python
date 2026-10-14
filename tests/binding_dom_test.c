/* data-bind-* end to end through the public ABI (Tessera's data-binding spec § "Required dynamic conformance cases"):
 * values set before load reach the bindings when the document installs, later sets reach them on the
 * next advance, writes coalesce, unset paths and bad expressions leave the authored target, and a
 * reload re-applies the model to the new document. The page has one observer script, which only
 * reports the bound targets in document.title each frame; the bindings themselves run no script.
 * That title write changes the tree every frame, so every frame rescans: a bad expression is still
 * reported once per document. A third page inserts bound markup by script after load.
 * Built as karouka_binding_dom_test (C, public header only) and run by tools/dev.sh check. */

#include <karouka/karouka.h>

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static char const page[] =
    "<!doctype html><title>loading</title><body>"
    "<p id=p class=a data-bind-text=\"'hp ' + player.health\" data-bind-class-low=\"player.health < 30\""
    " data-bind-style-width=\"player.health + '%'\">authored</p>"
    "<input id=i value=x data-bind-value=\"player.health * 2\">"
    "<p id=q data-bind-text=\"player.name\">unset</p>"
    "<p id=bad data-bind-text=\"1 +\">bad</p>"
    "<p id=deep data-bind-text=\"((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))\">deep</p>"
    "<p id=dead data-bind-text=\"player.dead ? 'dead' : 'alive'\">authored</p>"
    "<script>"
    "function report() {"
    "  var p = document.getElementById('p');"
    "  document.title = location.pathname.slice(-1) + ':' + [p.textContent, p.className, p.style.width, document.getElementById('i').value,"
    "    document.getElementById('q').textContent, document.getElementById('bad').textContent,"
    "    document.getElementById('deep').textContent, document.getElementById('dead').textContent].join('|');"
    "  requestAnimationFrame(report);"
    "}"
    "requestAnimationFrame(report);"
    "</script></body>";

/* Markup added after the document completed: an appended element, an attribute set on an existing
 * one, and a bad expression, which is reported once. */
static char const late_page[] =
    "<!doctype html><title>loading</title><body><div id=box><p id=host>host</p></div>"
    "<script>"
    "var when = 'none';"
    "function report() {"
    "  var late = document.getElementById('late');"
    "  document.title = 'c:' + when + '|' + (late ? late.textContent : 'none') + '|' + document.getElementById('host').textContent;"
    "  requestAnimationFrame(report);"
    "}"
    "requestAnimationFrame(report);"
    "addEventListener('load', function() {"
    "  setTimeout(function() {"
    "    when = document.readyState;"
    "    var p = document.createElement('p');"
    "    p.id = 'late';"
    "    p.textContent = 'authored';"
    "    p.setAttribute('data-bind-text', \"'late ' + player.health\");"
    "    document.getElementById('box').appendChild(p);"
    "    document.getElementById('host').setAttribute('data-bind-text', \"'host ' + player.health\");"
    "    var bad = document.createElement('p');"
    "    bad.setAttribute('data-bind-text', '2 *');"
    "    document.body.appendChild(bad);"
    "  }, 0);"
    "});"
    "</script></body>";

static int disabled_warnings;

static void count_log(void* user_data, kr_log_level level, char const* category, char const* message)
{
    (void)user_data;
    if (level == KR_LOG_WARN && strcmp(category, "karouka") == 0 && strstr(message, " disabled: "))
        ++disabled_warnings;
}

static double clock_ms;
static char last_title[512];

/* Drives frames until the title reads `expected`; returns whether it did within 60 frames. */
static int settle(kr_view* view, char const* expected)
{
    for (int frame = 0; frame < 60; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        if (kr_view_advance_time(view, clock_ms) != KR_OK || kr_view_render(view, &rendered) != KR_OK)
            return 0;
        snprintf(last_title, sizeof last_title, "%s", kr_view_title(view));
        if (strcmp(last_title, expected) == 0)
            return 1;
    }
    fprintf(stderr, "  expected '%s'\n  got      '%s'\n", expected, last_title);
    return 0;
}

int main(void)
{
    kr_host_interface host = { 0 };
    host.struct_size = sizeof host;
    host.log.write = count_log;
    host.log.min_level = KR_LOG_WARN;
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    system_desc.host = &host;
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_binding_dom_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    kr_binding_path health = 0, name = 0, dead = 0;
    CHECK(kr_binding_path_resolve(view, "player.health", &health) == KR_OK);
    CHECK(kr_binding_path_resolve(view, "player.name", &name) == KR_OK);
    CHECK(kr_binding_path_resolve(view, "player.dead", &dead) == KR_OK);

    /* Set before the document exists: applied when it installs. Unset player.name and player.dead
     * and the two bad expressions leave their authored text. */
    CHECK(kr_binding_set_number(view, health, 50) == KR_OK);
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-binding-dom-test/a") == KR_OK);
    CHECK(settle(view, "a:hp 50|a|50%|100|unset|bad|deep|authored"));

    /* Several writes before one advance: the last one lands. */
    CHECK(kr_binding_set_number(view, health, 10) == KR_OK);
    CHECK(kr_binding_set_number(view, health, 25) == KR_OK);
    CHECK(settle(view, "a:hp 25|a low|25%|50|unset|bad|deep|authored"));

    /* A string the caller then overwrites; a bool that selects a ternary arm. */
    char caller[4] = "Ada";
    CHECK(kr_binding_set_string(view, name, caller, 3) == KR_OK);
    caller[0] = 'X';
    CHECK(kr_binding_set_bool(view, dead, true) == KR_OK);
    CHECK(settle(view, "a:hp 25|a low|25%|50|Ada|bad|deep|dead"));

    /* Back above the threshold: the class goes, everything else follows. */
    CHECK(kr_binding_set_number(view, health, 82.5) == KR_OK);
    CHECK(settle(view, "a:hp 82.5|a|82.5%|165|Ada|bad|deep|dead"));

    /* Reload: a new document, the same model, applied in full. */
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-binding-dom-test/b") == KR_OK);
    CHECK(settle(view, "b:hp 82.5|a|82.5%|165|Ada|bad|deep|dead"));
    CHECK(kr_binding_set_bool(view, dead, false) == KR_OK);
    CHECK(settle(view, "b:hp 82.5|a|82.5%|165|Ada|bad|deep|alive"));
    /* Two bad expressions per document, rescanned every frame, each reported once. */
    CHECK(disabled_warnings == 4);

    /* Bound markup inserted by script after load picks up the current value, then follows it. */
    disabled_warnings = 0;
    CHECK(kr_view_load_html(view, late_page, strlen(late_page), "file:///karouka-binding-dom-test/c") == KR_OK);
    CHECK(settle(view, "c:complete|late 82.5|host 82.5"));
    CHECK(kr_binding_set_number(view, health, 20) == KR_OK);
    CHECK(settle(view, "c:complete|late 20|host 20"));
    CHECK(disabled_warnings == 1);

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures) {
        fprintf(stderr, "karouka_binding_dom_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("karouka_binding_dom_test: ok\n");
    return 0;
}
