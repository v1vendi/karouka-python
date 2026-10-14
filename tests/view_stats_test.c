/* kr_view_stats is versioned by struct_size: kr_view_query_stats writes only the members
 * that fit in the host's struct_size, so a host built against an older, smaller header gets the
 * members it knows and nothing past its struct. The kr_view_get_stats symbol that hosts built
 * against the headers before struct_size call writes only those headers' common 48 bytes.
 * Each case hands karouka a buffer filled with a sentinel and checks which bytes changed.
 * Built as karouka_view_stats_test (C, public header only). */

#include <karouka/karouka.h>

#include <stddef.h>
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

/* What a host built against a header Previously, calls: no struct_size, 48 or 56 bytes. */
kr_result kr_view_get_stats(kr_view* view, void* out_stats);

#define SENTINEL 0xAB
#define BUFFER_BYTES (sizeof(kr_view_stats) + 32)

/* 8-byte aligned, as a host's struct would be. */
static union {
    kr_view_stats stats;
    unsigned char bytes[BUFFER_BYTES];
} buffer;

static void fill(uint32_t struct_size)
{
    memset(buffer.bytes, SENTINEL, sizeof buffer.bytes);
    buffer.stats.struct_size = struct_size;
}

/* Whether bytes [from, BUFFER_BYTES) all still hold the sentinel. */
static int untouched_from(size_t from)
{
    for (size_t i = from; i < BUFFER_BYTES; ++i) {
        if (buffer.bytes[i] != SENTINEL)
            return 0;
    }
    return 1;
}

static uint64_t u64_at(size_t offset)
{
    uint64_t value;
    memcpy(&value, buffer.bytes + offset, sizeof value);
    return value;
}

static double clock_ms;

static void run_frames(kr_view* view, int frames)
{
    for (int frame = 0; frame < frames; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
    }
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_view_stats_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    static char const page[] = "<!doctype html><body style='background:rgb(0,0,255)'><p>stats</p></body>";
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-view-stats-test/") == KR_OK);
    run_frames(view, 10);

    size_t const frames_at = offsetof(kr_view_stats, frames_rendered);
    size_t const updates_at = offsetof(kr_view_stats, updates_run);
    size_t const layouts_at = offsetof(kr_view_stats, layout_updates);
    size_t const pumps_at = offsetof(kr_view_stats, event_loop_pumps);

    /* The current header: every member, nothing past the struct. */
    fill(sizeof(kr_view_stats));
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_OK);
    kr_view_stats const full = buffer.stats;
    CHECK(full.struct_size == sizeof(kr_view_stats));
    CHECK(full.frames_rendered >= 1 && full.frames_rendered <= 10);
    CHECK(full.updates_run >= 1 && full.updates_run <= 10);
    CHECK(full.layout_updates >= 1 && full.layout_updates < 1000);
    CHECK(full.event_loop_pumps >= 10);
    CHECK(untouched_from(sizeof(kr_view_stats)));

    /* A header from before event_loop_pumps: layout_updates is its last member. */
    fill((uint32_t)pumps_at);
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_OK);
    CHECK(buffer.stats.layout_updates == full.layout_updates);
    CHECK(untouched_from(pumps_at));

    /* A header from before layout_updates: the members before it, and not one byte more. */
    fill((uint32_t)layouts_at);
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_OK);
    CHECK(buffer.stats.struct_size == layouts_at);
    CHECK(buffer.stats.frames_rendered == full.frames_rendered);
    CHECK(buffer.stats.updates_run == full.updates_run);
    CHECK(untouched_from(layouts_at));

    /* A size between members gets whole members only. */
    fill((uint32_t)(layouts_at + 4));
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_OK);
    CHECK(untouched_from(layouts_at));
    fill((uint32_t)(updates_at + 7));
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_OK);
    CHECK(u64_at(frames_at) == full.frames_rendered);
    CHECK(untouched_from(updates_at));

    /* A newer header's larger struct: the members this library knows; the rest stays the host's. */
    fill((uint32_t)BUFFER_BYTES);
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_OK);
    CHECK(buffer.stats.layout_updates == full.layout_updates);
    CHECK(buffer.stats.event_loop_pumps == full.event_loop_pumps);
    CHECK(untouched_from(sizeof(kr_view_stats)));

    /* No size (a zeroed struct), or too small for frames_rendered: refused, nothing written. */
    fill(0);
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(untouched_from(sizeof(uint32_t)));
    fill((uint32_t)(frames_at + 7));
    CHECK(kr_view_query_stats(view, &buffer.stats) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(untouched_from(sizeof(uint32_t)));
    fill(sizeof(kr_view_stats));
    CHECK(kr_view_query_stats(NULL, &buffer.stats) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_query_stats(view, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(untouched_from(sizeof(uint32_t)));

    /* A host built before struct_size: the old layout's 48 bytes, frames_rendered first, nothing
     * past them, whatever its struct's first bytes held. */
    memset(buffer.bytes, SENTINEL, sizeof buffer.bytes);
    CHECK(kr_view_get_stats(view, buffer.bytes) == KR_OK);
    CHECK(u64_at(0) == full.frames_rendered);
    CHECK(u64_at(8) == full.updates_run);
    CHECK(untouched_from(48));
    CHECK(kr_view_get_stats(view, NULL) == KR_ERROR_INVALID_ARGUMENT);

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures)
        fprintf(stderr, "karouka_view_stats_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
