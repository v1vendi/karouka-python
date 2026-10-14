/* The `engine` page <-> host channel through the public ABI (Tessera's ): page
 * triggers and calls reach the host callbacks in order with JSON arguments; host triggers become
 * engine.on calls and host resolves settle engine.call, delivered on the next advance; answered,
 * unknown and previous-document call ids are refused; the queue limit refuses without enqueueing;
 * a reload discards pending calls; host JSON over KR_ENGINE_MAX_JSON_BYTES is refused, for a
 * trigger and for a resolve, which leaves the call pending; an
 * iframe has no document, so no `engine` of its own, and host triggers reach only the page's
 *. The page reports what it received in document.title each frame.
 * Built as karouka_engine_channel_test (C, public header only) and run by tools/dev.sh check. */

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
    "<!doctype html><title>loading</title><body><script>"
    "var log = [];"
    "engine.on('ping', function (a, b) { log.push('ping:' + a + ',' + b); });"
    "engine.trigger('hello', 1, { x: 2 });"
    "engine.call('ask', 'q').then(function (v) { log.push('ask:' + JSON.stringify(v)); });"
    "engine.call('fail').catch(function (e) { log.push('fail:' + e.message); });"
    "engine.call('later').then(function () { log.push('later'); }, function () { log.push('later-rejected'); });"
    "function report() { document.title = log.join('|') || 'none'; requestAnimationFrame(report); }"
    "requestAnimationFrame(report);"
    "</script></body>";

/* karouka has no child navigables: the iframe has no document or Window. */
static char const framed_page[] =
    "<!doctype html><title>loading</title><body><script>"
    "var log = [];"
    "engine.on('ping', function (a) { log.push('top:' + a); });"
    "function report() { document.title = log.join('|') || 'none'; requestAnimationFrame(report); }"
    "requestAnimationFrame(report);"
    "var frame = document.createElement('iframe');"
    "frame.srcdoc = '<script>parent.engine.trigger(\\'framed\\');<\\/script>';"
    "document.body.appendChild(frame);"
    "engine.trigger('inert', frame.contentDocument === null, frame.contentWindow === null);"
    "</script></body>";

/* What the host saw, in arrival order: "t:name:json" and "c:name:json". */
static char host_log[2048];
static uint64_t call_ids[8];
static int call_count;

static void append(char kind, char const* name, char const* json)
{
    size_t used = strlen(host_log);
    snprintf(host_log + used, sizeof host_log - used, "%s%c:%s:%s", used ? "|" : "", kind, name, json);
}

static void on_trigger(void* user_data, kr_view* view, char const* name, char const* json_args)
{
    (void)view;
    CHECK(user_data == &call_count);
    append('t', name, json_args);
}

static void on_call(void* user_data, kr_view* view, char const* name, char const* json_args, uint64_t call_id)
{
    (void)view;
    CHECK(user_data == &call_count);
    append('c', name, json_args);
    if (call_count < 8)
        call_ids[call_count++] = call_id;
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
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_engine_channel_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    kr_engine_callbacks callbacks = { sizeof(kr_engine_callbacks), on_trigger, on_call, &call_count };
    CHECK(kr_view_set_engine_callbacks2(view, &callbacks) == KR_OK);

    CHECK(kr_view_engine_trigger(NULL, "x", NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_engine_trigger(view, NULL, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_engine_resolve(NULL, 1, true, NULL) == KR_ERROR_INVALID_ARGUMENT);

    /* Page -> host: synchronous, in order, the argument array as JSON. */
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-engine-channel-test/a") == KR_OK);
    CHECK(settle(view, "none"));
    CHECK(strcmp(host_log, "t:hello:[1,{\"x\":2}]|c:ask:[\"q\"]|c:fail:[]|c:later:[]") == 0);
    if (strcmp(host_log, "t:hello:[1,{\"x\":2}]|c:ask:[\"q\"]|c:fail:[]|c:later:[]") != 0)
        fprintf(stderr, "  host saw '%s'\n", host_log);
    CHECK(call_count == 3);

    /* Bad names and ids are refused; nothing about them reaches the page. */
    char long_name[300];
    memset(long_name, 'n', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    CHECK(kr_view_engine_trigger(view, "", NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_engine_trigger(view, long_name, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_engine_resolve(view, 999999, true, NULL) == KR_ERROR_INVALID_ARGUMENT);

    /* Host -> page outside a callback: queued, delivered in call order on the next advance. */
    CHECK(kr_view_engine_trigger(view, "ping", "[3,\"four\"]") == KR_OK);
    CHECK(kr_view_engine_resolve(view, call_ids[0], true, "{\"a\":1}") == KR_OK);
    CHECK(kr_view_engine_resolve(view, call_ids[0], true, "{\"a\":1}") == KR_ERROR_INVALID_ARGUMENT);
    CHECK(kr_view_engine_resolve(view, call_ids[1], false, "\"nope\"") == KR_OK);
    CHECK(settle(view, "ping:3,four|ask:{\"a\":1}|fail:nope"));

    /* The queue refuses past KR_ENGINE_MAX_QUEUED_MESSAGES without enqueueing; the rest arrive. */
    int accepted = 0;
    for (int i = 0; i < KR_ENGINE_MAX_QUEUED_MESSAGES + 1; ++i) {
        if (kr_view_engine_trigger(view, "flood", NULL) == KR_OK)
            ++accepted;
    }
    CHECK(accepted == KR_ENGINE_MAX_QUEUED_MESSAGES);
    CHECK(kr_view_engine_trigger(view, "flood", NULL) == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(settle(view, "ping:3,four|ask:{\"a\":1}|fail:nope"));
    CHECK(kr_view_engine_trigger(view, "flood", NULL) == KR_OK);

    /* Host JSON over KR_ENGINE_MAX_JSON_BYTES is refused before anything is copied or queued. */
    static char big[KR_ENGINE_MAX_JSON_BYTES + 2];
    memset(big, ' ', sizeof big - 1);
    big[0] = '[';
    big[KR_ENGINE_MAX_JSON_BYTES - 1] = ']';
    big[KR_ENGINE_MAX_JSON_BYTES] = '\0';
    CHECK(kr_view_engine_trigger(view, "big", big) == KR_OK);
    big[KR_ENGINE_MAX_JSON_BYTES] = ' ';
    big[KR_ENGINE_MAX_JSON_BYTES + 1] = '\0';
    CHECK(kr_view_engine_trigger(view, "big", big) == KR_ERROR_LIMIT_EXCEEDED);
    CHECK(settle(view, "ping:3,four|ask:{\"a\":1}|fail:nope"));

    /* A reload discards the pending call: its id is refused, the new document's calls are fresh. */
    uint64_t stale = call_ids[2];
    host_log[0] = '\0';
    call_count = 0;
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-engine-channel-test/b") == KR_OK);
    CHECK(settle(view, "none"));
    CHECK(kr_view_engine_resolve(view, stale, true, NULL) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(call_count == 3 && call_ids[0] > stale);
    /* A resolve over KR_ENGINE_MAX_JSON_BYTES is refused and leaves its call pending; one
     * at the cap is taken ('fail' resolves, so its catch logs nothing). */
    CHECK(kr_view_engine_resolve(view, call_ids[2], true, big) == KR_ERROR_LIMIT_EXCEEDED);
    big[KR_ENGINE_MAX_JSON_BYTES] = '\0';
    CHECK(kr_view_engine_resolve(view, call_ids[1], true, big) == KR_OK);
    CHECK(kr_view_engine_resolve(view, call_ids[2], true, NULL) == KR_OK);
    CHECK(settle(view, "later"));

    /* Without callbacks engine.trigger does nothing and engine.call rejects. */
    CHECK(kr_view_set_engine_callbacks2(view, NULL) == KR_OK);
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-engine-channel-test/c") == KR_OK);
    CHECK(settle(view, "fail:engine.call: the host handles no calls|later-rejected"));
    CHECK(call_count == 3);

    /* An iframe has no document: its srcdoc's script never runs, and host triggers
     * reach the page's one `engine`. */
    CHECK(kr_view_set_engine_callbacks2(view, &callbacks) == KR_OK);
    host_log[0] = '\0';
    call_count = 0;
    CHECK(kr_view_load_html(view, framed_page, strlen(framed_page), "file:///karouka-engine-channel-test/d") == KR_OK);
    CHECK(settle(view, "none"));
    CHECK(strcmp(host_log, "t:inert:[true,true]") == 0);
    if (strcmp(host_log, "t:inert:[true,true]") != 0)
        fprintf(stderr, "  host saw '%s'\n", host_log);
    CHECK(kr_view_engine_trigger(view, "ping", "[7]") == KR_OK);
    CHECK(settle(view, "top:7"));

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures)
        fprintf(stderr, "karouka_engine_channel_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
