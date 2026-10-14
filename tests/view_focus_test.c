/* kr_view_set_focused: an unfocused view paints its selection in the palette's
 * InactiveSelection, a focused one in Selection, and the change repaints the page on the next frame.
 * Unfocused, document.hasFocus() is false and a focused field's caret stops blinking (no frame
 * renders for it); the state lasts across a load. A change fires blur or focus at the window and the
 * focused field. Built as karouka_view_focus_test (C, public header only). */

#include "test_util.h"

#include <karouka/karouka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static double clock_ms;

/* The palette's colors (Engine.cpp's PageClient: Selection #84351a, InactiveSelection #606060) as
 * they show over white. A selection wash is made translucent, at the lowest alpha from 60% to 80%
 * that still looks like the palette color over white; the gray reaches it, Selection's blue can't
 * (it would need a negative channel) and shows lighter. */
enum {
    SELECTION = 0x843533,
    INACTIVE_SELECTION = 0x606060,
};

/* The last frame's pixel at (x, y) as 0xRRGGBB, or ~0u if the frame can't be read. */
static unsigned frame_pixel(kr_view* view, int x, int y)
{
    void const* pixels = NULL;
    size_t bytes_per_row = 0;
    if (kr_view_lock_pixels(view, &pixels, &bytes_per_row) != KR_OK)
        return ~0u;
    unsigned char const* p = (unsigned char const*)pixels + (size_t)y * bytes_per_row + (size_t)x * 4;
    unsigned rgb = ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
    kr_view_unlock_pixels(view);
    return rgb;
}

/* Whether every channel of `rgb` is within 6 of `expected`'s (the wash is blended at 60-80% alpha). */
static int near_color(unsigned rgb, unsigned expected)
{
    for (int shift = 0; shift <= 16; shift += 8) {
        int have = (int)((rgb >> shift) & 0xff);
        int want = (int)((expected >> shift) & 0xff);
        if (abs(have - want) > 6)
            return 0;
    }
    return 1;
}

static void expect_pixel(kr_view* view, unsigned expected, char const* what, int line)
{
    unsigned rgb = frame_pixel(view, 8, 20);
    if (!near_color(rgb, expected)) {
        fprintf(stderr, "%s:%d: %s: pixel (8, 20) is #%06x, want about #%06x\n", __FILE__, line, what, rgb, expected);
        ++failures;
    }
}

/* Frames rendered out of `frames`, each 16 ms on. */
static int rendered_frames(kr_view* view, int frames)
{
    int count = 0;
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
        count += rendered;
    }
    return count;
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_view_focus_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 128, .height = 64, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    if (!view) {
        kr_system_destroy(system);
        return 1;
    }

    CHECK(kr_view_set_focused(NULL, false) == KR_ERROR_INVALID_ARGUMENT);

    /* Unfocused before the load: the page loads into an unfocused view. Its selection covers
     * no-break spaces, which have no ink, so (8, 20) shows the wash alone. */
    CHECK(kr_view_set_focused(view, false) == KR_OK);
    static char const selection_page[] =
        "<!doctype html><style>body { margin: 0; background: white } p { margin: 0; font: 40px/40px sans-serif }</style>"
        "<p id=p>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</p><script>"
        "getSelection().selectAllChildren(document.getElementById('p'));"
        "function tick() { document.title = document.hasFocus() ? 'focused' : 'blurred'; requestAnimationFrame(tick); }"
        "tick();"
        "</script>";
    CHECK(kr_view_load_html(view, selection_page, strlen(selection_page), "file:///karouka-view-focus-test/") == KR_OK);
    CHECK(settle_title(view, &clock_ms, "blurred"));
    expect_pixel(view, INACTIVE_SELECTION, "unfocused view", __LINE__);

    /* Focusing repaints the selection in the active color. */
    CHECK(kr_view_set_focused(view, true) == KR_OK);
    CHECK(settle_title(view, &clock_ms, "focused"));
    expect_pixel(view, SELECTION, "focused view", __LINE__);

    /* And unfocusing an already painted page repaints it in the inactive color; setting the same
     * state again changes nothing. */
    CHECK(kr_view_set_focused(view, false) == KR_OK);
    CHECK(kr_view_set_focused(view, false) == KR_OK);
    CHECK(settle_title(view, &clock_ms, "blurred"));
    expect_pixel(view, INACTIVE_SELECTION, "unfocused again", __LINE__);

    /* The window focus change fires trusted FocusEvents along the focus chain: losing focus,
     * blur and focusout at the focused field, then blur at the window; gaining it, focus at the
     * window, then focus and focusin at the field. The field stays document.activeElement, and only
     * a change of state fires anything. Each entry is target:type, flagged if the event isn't a
     * trusted, composed FocusEvent with no relatedTarget and the window as view; a window entry
     * adds the active element and document.hasFocus(). (It is told by currentTarget: LibWeb gives
     * every event dispatched at a Window the document as its target, LibWeb
     * pitfalls.) */
    CHECK(kr_view_set_focused(view, true) == KR_OK);
    static char const events_page[] =
        "<!doctype html><input id=field><script>"
        "const field = document.getElementById('field'); field.focus(); const log = [];"
        "function record(e) {"
        "  if (e.eventPhase !== Event.AT_TARGET) return;"
        "  let entry = (e.currentTarget === window ? 'window' : e.target.id) + ':' + e.type;"
        "  if (!e.isTrusted || !(e instanceof FocusEvent) || !e.composed || e.relatedTarget !== null || e.view !== window"
        "      || e.bubbles !== (e.type === 'focusin' || e.type === 'focusout')) entry += '!';"
        "  if (e.currentTarget === window) entry += '(' + document.activeElement.id + ',' + document.hasFocus() + ')';"
        "  log.push(entry); document.title = log.join(' ');"
        "}"
        "for (const type of ['focus', 'blur', 'focusin', 'focusout']) { window.addEventListener(type, record); field.addEventListener(type, record); }"
        "document.title = 'ready';"
        "</script>";
    CHECK(kr_view_load_html(view, events_page, strlen(events_page), "file:///karouka-view-focus-test/") == KR_OK);
    CHECK(settle_title(view, &clock_ms, "ready"));
    CHECK(kr_view_set_focused(view, false) == KR_OK);
    CHECK(settle_title(view, &clock_ms, "field:blur field:focusout window:blur(field,false)"));
    CHECK(kr_view_set_focused(view, false) == KR_OK);
    CHECK(kr_view_set_focused(view, true) == KR_OK);
    CHECK(kr_view_set_focused(view, true) == KR_OK);
    CHECK(settle_title(view, &clock_ms,
        "field:blur field:focusout window:blur(field,false) window:focus(field,true) field:focus field:focusin"));

    /* A focused text field's caret blinks, so a focused view renders for it with nothing else
     * changing; an unfocused one draws no caret and renders nothing. */
    CHECK(kr_view_set_focused(view, true) == KR_OK);
    static char const caret_page[] =
        "<!doctype html><input id=field><script>"
        "document.getElementById('field').focus(); document.title = 'ready';"
        "</script>";
    CHECK(kr_view_load_html(view, caret_page, strlen(caret_page), "file:///karouka-view-focus-test/") == KR_OK);
    CHECK(settle_title(view, &clock_ms, "ready"));
    int focused_renders = rendered_frames(view, 125);
    CHECK(focused_renders >= 2);
    CHECK(kr_view_set_focused(view, false) == KR_OK);
    CHECK(rendered_frames(view, 2) >= 1); /* the repaint without the caret */
    int unfocused_renders = rendered_frames(view, 125);
    if (unfocused_renders != 0) {
        fprintf(stderr, "%s:%d: an unfocused view rendered %d of 125 frames of an idle page (focused: %d)\n", __FILE__, __LINE__, unfocused_renders, focused_renders);
        ++failures;
    }

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures) {
        fprintf(stderr, "karouka_view_focus_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_view_focus_test: ok\n");
    return 0;
}
