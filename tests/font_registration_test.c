/* The host's font registration through the public ABI: a face registered from memory
 * under a new family renders that family instead of San Francisco, and local() finds it; a face
 * registered as KR_FONT_FALLBACK renders a script SF lacks (Arabic) instead of SF's missing-glyph
 * boxes; kr_system_set_default_font puts a registered family in SF's place for generic and unknown
 * families, and NULL restores SF. kr_system_add_font_file2 goes through the host io, completed both
 * inside the request and later. A document loaded before the registrations sees them without a
 * reload, and so does a default font set or cleared after it loaded. system-ui, ui-serif, ui-monospace, ui-rounded and the other system UI names
 * follow SF or the default family like every generic family, not CoreText. The page reports text widths in document.title.
 * Built as karouka_font_registration_test (C, public header only). */

#include <karouka/karouka.h>

#include <math.h>
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

/* pages/fonts/NAME in $KR_SOURCE_DIR (the source tree under test), else in the source it was built
 * from. Four results live at once. */
static char const* font_file(char const* name)
{
    static char paths[4][4096];
    static unsigned next;
    char const* root = getenv("KR_SOURCE_DIR");
    char* path = paths[next++ % 4];
    snprintf(path, sizeof paths[0], "%s/pages/fonts/%s", root && *root ? root : KAROUKA_SOURCE_DIR, name);
    return path;
}

/* Widths, in CSS px, of the same text in several families (see `report` below). */
enum { HOST, SANS, SERIF, UNKNOWN, LOCAL, ARABIC_SANS, ARABIC_HOST,
    SYSTEM_UI, UI_SANS_SERIF, UI_SERIF, UI_MONOSPACE, UI_ROUNDED, APPLE_SYSTEM, QUOTED_UI_MONOSPACE, WIDTH_COUNT };

static char const page[] =
    "<!doctype html><title>loading</title><style>"
    "@font-face { font-family: LocalSans; src: local(\"NotoSans-Regular\"); }"
    "span { font-size: 40px; white-space: pre; }"
    "</style><body>"
    "<span style='font-family: HostSans'>Hamburgefonstiv</span>"
    "<span style='font-family: sans-serif'>Hamburgefonstiv</span>"
    "<span style='font-family: serif'>Hamburgefonstiv</span>"
    "<span style='font-family: NoSuchFamily'>Hamburgefonstiv</span>"
    "<span style='font-family: LocalSans, monospace'>Hamburgefonstiv</span>"
    "<span style='font-family: sans-serif'>\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85</span>"
    "<span style='font-family: HostArabic'>\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85</span>"
    "<span style='font-family: system-ui'>Hamburgefonstiv</span>"
    "<span style='font-family: ui-sans-serif'>Hamburgefonstiv</span>"
    "<span style='font-family: ui-serif'>Hamburgefonstiv</span>"
    "<span style='font-family: ui-monospace'>Hamburgefonstiv</span>"
    "<span style='font-family: ui-rounded'>Hamburgefonstiv</span>"
    "<span style='font-family: -apple-system'>Hamburgefonstiv</span>"
    "<span style='font-family: \"ui-monospace\"'>Hamburgefonstiv</span>"
    "<script>"
    "function report() {"
    "  document.title = Array.from(document.querySelectorAll('span'), s => s.getBoundingClientRect().width.toFixed(2)).join(' ');"
    "  requestAnimationFrame(report);"
    "}"
    "requestAnimationFrame(report);"
    "</script></body>";

static double clock_ms;

/* Drives 30 frames of the document `view` shows and reads the widths from the title. */
static int settle(kr_view* view, double widths[WIDTH_COUNT])
{
    for (int frame = 0; frame < 30; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        if (kr_view_advance_time(view, clock_ms) != KR_OK || kr_view_render(view, &rendered) != KR_OK)
            return 0;
    }
    char const* title = kr_view_title(view);
    char const* cursor = title;
    for (int i = 0; i < WIDTH_COUNT; ++i) {
        char* end = NULL;
        widths[i] = strtod(cursor, &end);
        if (end == cursor) {
            fprintf(stderr, "  unreadable title '%s'\n", title);
            return 0;
        }
        cursor = end;
    }
    fprintf(stderr, "  widths: %s\n", title);
    return 1;
}

/* Loads the page into `view` afresh and measures it. */
static int measure(kr_view* view, double widths[WIDTH_COUNT])
{
    if (kr_view_load_html(view, page, strlen(page), "file:///karouka-font-registration-test/") != KR_OK)
        return 0;
    return settle(view, widths);
}

static int same(double a, double b)
{
    return fabs(a - b) < 0.5;
}

/* system-ui, the ui-* generic families, -apple-system and a quoted "ui-monospace" render in `width`'s
 * font: the provider's (SF, or the host's default family), never CoreText's SF Mono, New York or
 * SF Rounded. */
static void check_system_ui_families(double const widths[WIDTH_COUNT], double width)
{
    for (int i = SYSTEM_UI; i < WIDTH_COUNT; ++i) {
        if (!same(widths[i], width)) {
            fprintf(stderr, "  system-ui family #%d: %.2f, expected %.2f\n", i - SYSTEM_UI, widths[i], width);
            CHECK(same(widths[i], width));
        }
    }
}

static void* read_file(char const* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

/* The host io: reads file: URLs from disk, inside the request or, when `defer` is set, later. */
static kr_system* the_system;
static int defer;
static uint64_t deferred_id;
static char deferred_path[1024];

static void complete_from_disk(uint64_t id, char const* path)
{
    size_t size = 0;
    void* data = read_file(path, &size);
    if (data) {
        CHECK(kr_io_complete(the_system, id, KR_OK, data, size, KR_IO_OWNERSHIP_COPY) == KR_OK);
        free(data);
    } else {
        CHECK(kr_io_complete(the_system, id, KR_ERROR_IO, NULL, 0, KR_IO_OWNERSHIP_COPY) == KR_OK);
    }
}

static kr_result io_request(void* user_data, char const* url, uint64_t id)
{
    (void)user_data;
    if (strncmp(url, "file://", 7) != 0)
        return KR_ERROR_IO;
    if (defer) {
        deferred_id = id;
        snprintf(deferred_path, sizeof deferred_path, "%s", url + 7);
        return KR_OK;
    }
    complete_from_disk(id, url + 7);
    return KR_OK;
}

int main(void)
{
    kr_host_interface host = { 0 };
    host.struct_size = sizeof host;
    host.io.request = io_request;
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    system_desc.host = &host;
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_font_registration_test: engine failed to start\n");
        return 1;
    }
    the_system = system;
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 800;
    view_desc.height = 200;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    /* Nothing registered: every family is SF, and Arabic is SF's missing-glyph boxes. */
    double before[WIDTH_COUNT];
    CHECK(measure(view, before));
    CHECK(same(before[HOST], before[SANS]));
    CHECK(same(before[SERIF], before[SANS]));
    CHECK(same(before[LOCAL], before[SANS])); /* local() found nothing: the monospace fallback, SF */
    CHECK(same(before[ARABIC_HOST], before[ARABIC_SANS]));
    check_system_ui_families(before, before[SANS]);

    /* Refusals register nothing. */
    static unsigned char const not_a_font[64] = { 1, 2, 3 };
    kr_font_desc host_sans = { sizeof(kr_font_desc), "HostSans", 0, KR_FONT_FLAGS_NONE };
    kr_font_desc empty_family = { sizeof(kr_font_desc), "", 0, KR_FONT_FLAGS_NONE };
    CHECK(kr_system_add_font2(NULL, not_a_font, sizeof not_a_font, &host_sans) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_add_font2(system, NULL, 10, &host_sans) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_add_font2(system, not_a_font, sizeof not_a_font, &host_sans) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_add_font_file2(system, NULL, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_add_font_file2(system, font_file("no-such-font.ttf"), NULL) == KR_ERROR_IO);
    CHECK(kr_system_set_default_font(system, "HostSans") == KR_ERROR_INVALID_ARGUMENT);

    /* From memory, under a family of the host's naming; the host frees its copy at once. */
    size_t size = 0;
    void* noto_sans = read_file(font_file("NotoSans-Regular.ttf"), &size);
    CHECK(noto_sans != NULL);
    CHECK(kr_system_add_font2(system, noto_sans, size, &empty_family) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_system_add_font2(system, noto_sans, size, &host_sans) == KR_OK);
    free(noto_sans);

    /* From a file through the host io, completed later, as a glyph fallback. */
    kr_font_desc host_arabic = { sizeof(kr_font_desc), "HostArabic", 0, KR_FONT_FALLBACK };
    defer = 1;
    CHECK(kr_system_add_font_file2(system, font_file("NotoSansArabic-Regular.ttf"), &host_arabic) == KR_OK);
    defer = 0;
    CHECK(kr_system_set_default_font(system, "HostArabic") == KR_ERROR_INVALID_ARGUMENT); /* not yet */
    complete_from_disk(deferred_id, deferred_path);
    /* And completed inside the request, under the font's own family name. */
    CHECK(kr_system_add_font_file2(system, font_file("NotoSansDevanagari-Regular.ttf"), NULL) == KR_OK);
    CHECK(kr_system_set_default_font(system, "Noto Sans Devanagari") == KR_OK);
    CHECK(kr_system_set_default_font(system, NULL) == KR_OK);

    /* The document loaded before the registrations sees them without a reload: it had
     * resolved HostSans and HostArabic to SF, and shaped the Arabic as missing-glyph boxes. */
    double live[WIDTH_COUNT];
    CHECK(settle(view, live));
    CHECK(!same(live[HOST], before[HOST]));
    CHECK(!same(live[ARABIC_SANS], before[ARABIC_SANS]));

    double registered[WIDTH_COUNT];
    CHECK(measure(view, registered));
    for (int i = 0; i < WIDTH_COUNT; ++i) {
        if (i != LOCAL) /* local() is looked up when the @font-face rule loads, not again */
            CHECK(same(live[i], registered[i]));
    }
    CHECK(!same(registered[HOST], registered[SANS]));        /* HostSans is Noto Sans, not SF */
    CHECK(same(registered[SANS], before[SANS]));             /* sans-serif is still SF */
    CHECK(same(registered[LOCAL], registered[HOST]));        /* local("NotoSans-Regular") */
    CHECK(!same(registered[ARABIC_SANS], before[ARABIC_SANS])); /* no longer missing-glyph boxes */
    CHECK(same(registered[ARABIC_SANS], registered[ARABIC_HOST])); /* the fallback face */
    check_system_ui_families(registered, before[SANS]);

    /* A registered family in SF's place. The document loaded before sees it without a reload:
     * it had resolved the generic and unknown families to SF. */
    CHECK(kr_system_set_default_font(system, "HostSans") == KR_OK);
    double live_defaulted[WIDTH_COUNT];
    CHECK(settle(view, live_defaulted));
    CHECK(!same(live_defaulted[SANS], registered[SANS]));
    double defaulted[WIDTH_COUNT];
    CHECK(measure(view, defaulted));
    for (int i = 0; i < WIDTH_COUNT; ++i)
        CHECK(same(live_defaulted[i], defaulted[i]));
    CHECK(same(defaulted[SANS], registered[HOST]));
    CHECK(same(defaulted[SERIF], registered[HOST]));
    CHECK(same(defaulted[UNKNOWN], registered[HOST]));
    CHECK(same(defaulted[ARABIC_SANS], registered[ARABIC_HOST])); /* fallback still before last resort */
    check_system_ui_families(defaulted, registered[HOST]);

    /* And SF back. */
    CHECK(kr_system_set_default_font(system, NULL) == KR_OK);
    double live_restored[WIDTH_COUNT];
    CHECK(settle(view, live_restored));
    double restored[WIDTH_COUNT];
    CHECK(measure(view, restored));
    for (int i = 0; i < WIDTH_COUNT; ++i)
        CHECK(same(live_restored[i], restored[i]));
    CHECK(same(restored[SANS], before[SANS]));
    CHECK(same(restored[UNKNOWN], before[SANS]));
    check_system_ui_families(restored, before[SANS]);

    kr_view_destroy(view);
    kr_system_destroy(system);
    CHECK(kr_system_add_font2(system, not_a_font, sizeof not_a_font, NULL) == KR_ERROR_INVALID_ARGUMENT);

    if (failures)
        fprintf(stderr, "karouka_font_registration_test: %d check(s) failed\n", failures);
    else
        printf("karouka_font_registration_test: ok\n");
    return failures ? 1 : 0;
}
