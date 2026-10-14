/* kr_view_title follows the page that is loaded: a page without <title> reports "" rather than the
 * last page's title, a titled page after it reports its own, and a title script sets on a
 * titleless page shows up. Removing the <title>, inserting one already filled, or editing its text
 * node changes it too, and another document's <title> or SVG <title> does not. Built as karouka_title_reset_test (as C, against the public header only)
 * and run by tools/dev.sh check. */

#include <karouka/karouka.h>

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

/* Loads `html` and drives `frames` frames, so the parser and any script have run. */
static int load_and_run(kr_view* view, char const* html, int frames)
{
    if (kr_view_load_html(view, html, strlen(html), "file:///karouka-title-test/") != KR_OK)
        return 0;
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        if (kr_view_advance_time(view, clock_ms) != KR_OK || kr_view_render(view, &rendered) != KR_OK)
            return 0;
    }
    return 1;
}

static void expect_title(kr_view* view, char const* expected)
{
    char const* title = kr_view_title(view);
    if (strcmp(title, expected) != 0) {
        fprintf(stderr, "title is '%s', want '%s'\n", title, expected);
        ++failures;
    }
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_title_reset_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    expect_title(view, "");

    CHECK(load_and_run(view, "<!doctype html><title>first</title><p>one</p>", 10));
    expect_title(view, "first");

    /* The next page has no <title>: the view's title is the new document's, empty. */
    CHECK(load_and_run(view, "<!doctype html><p>two</p>", 10));
    expect_title(view, "");

    CHECK(load_and_run(view, "<!doctype html><title>third</title><p>three</p>", 10));
    expect_title(view, "third");

    /* A titleless page whose script sets the title later reports that title. */
    CHECK(load_and_run(view, "<!doctype html><p>four</p><script>requestAnimationFrame(() => { document.title = 'set'; });</script>", 10));
    expect_title(view, "set");

    /* And back to titleless once more, after a script-set title. */
    CHECK(load_and_run(view, "<!doctype html><p>five</p>", 10));
    expect_title(view, "");

    /* Removing the <title> element empties the title. */
    CHECK(load_and_run(view, "<!doctype html><title>gone</title><p>six</p>", 4));
    expect_title(view, "gone");
    CHECK(load_and_run(view, "<!doctype html><title>gone</title><script>requestAnimationFrame(() => document.querySelector('title').remove());</script>", 10));
    expect_title(view, "");

    /* Emptying it does too. */
    CHECK(load_and_run(view, "<!doctype html><title>full</title><script>requestAnimationFrame(() => { document.querySelector('title').textContent = ''; });</script>", 10));
    expect_title(view, "");

    /* A <title> filled before it is inserted, and an edit of its text node, both show up. */
    CHECK(load_and_run(view, "<!doctype html><p>seven</p><script>requestAnimationFrame(() => { const t = document.createElement('title'); t.textContent = 'late'; document.head.append(t); });</script>", 10));
    expect_title(view, "late");
    CHECK(load_and_run(view, "<!doctype html><title>before</title><script>requestAnimationFrame(() => { document.querySelector('title').firstChild.data = 'edited'; });</script>", 10));
    expect_title(view, "edited");
    /* Asked between edits, each answer is current. */
    CHECK(load_and_run(view, "<!doctype html><title>t0</title><script>let n = 0; function step() { document.querySelector('title').firstChild.data = 't' + ++n; if (n < 5) requestAnimationFrame(step); } requestAnimationFrame(step);</script>", 1));
    expect_title(view, "t1");
    for (int frame = 2; frame <= 5; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK && kr_view_render(view, &rendered) == KR_OK);
        char expected[8];
        snprintf(expected, sizeof expected, "t%d", frame);
        expect_title(view, expected);
    }

    /* Another document's title, HTML or SVG, is not the view's. (karouka has no iframes, 
     * a document the page creates stands in for a frame's.) */
    CHECK(load_and_run(view,
        "<!doctype html><title>top</title><body><script>"
        "const doc = document.implementation.createHTMLDocument('other');"
        "requestAnimationFrame(() => { doc.title = 'other-html'; });"
        "requestAnimationFrame(() => requestAnimationFrame(() => {"
        "  const svg = doc.createElementNS('http://www.w3.org/2000/svg', 'svg');"
        "  const title = doc.createElementNS('http://www.w3.org/2000/svg', 'title');"
        "  svg.append(title); doc.replaceChild(svg, doc.documentElement); title.textContent = 'other-svg';"
        "}));"
        "</script></body>",
        10));
    expect_title(view, "top");

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures) {
        fprintf(stderr, "karouka_title_reset_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_title_reset_test: ok\n");
    return 0;
}
