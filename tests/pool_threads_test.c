/* LibThreading's pool starts its threads as work arrives (patch 0041): a page whose only
 * off-thread job is one script compile runs it on one pool thread, not four (each has an 8 MiB stack),
 * and a page with no such job starts none. An idle pool thread exits after ~3 s of wall time without
 * work, and the next job starts one again (patch 0058). Counts this process's threads named
 * "Pool/N". Built as karouka_pool_threads_test (as C, against the public header only) and run by
 * tools/dev.sh check. */

#include "test_util.h"

#include <karouka/karouka.h>

#include <libproc.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static double clock_ms;

static int load(kr_view* view, char const* html)
{
    return kr_view_load_html(view, html, strlen(html), "file:///karouka-pool-threads-test/") == KR_OK;
}

/* This process's threads whose name starts with "Pool/", or -1 if they can't be listed. */
static int pool_thread_count(void)
{
    thread_act_array_t threads;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS)
        return -1;
    int pool = 0;
    for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
        thread_identifier_info_data_t id_info;
        mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id_info, &count) == KERN_SUCCESS) {
            struct proc_threadinfo info;
            if (proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, id_info.thread_id, &info, sizeof info) == (int)sizeof info
                && strncmp(info.pth_name, "Pool/", 5) == 0)
                ++pool;
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, thread_count * sizeof *threads);
    return pool;
}

int main(void)
{
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_pool_threads_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 64;
    view_desc.height = 64;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);

    /* No off-thread work yet: no pool threads. */
    CHECK(load(view, "<!doctype html><title>none</title><p>no script</p>"));
    CHECK(settle_title(view, &clock_ms, "none"));
    int before = pool_thread_count();
    if (before != 0) {
        fprintf(stderr, "%d pool threads before any off-thread job, want 0\n", before);
        ++failures;
    }

    /* One fetched classic script: one off-thread compile, one pool thread. */
    CHECK(load(view, "<!doctype html><title>x</title><script src=\"data:text/javascript,document.title%3D%22ran%22\"></script>"));
    CHECK(settle_title(view, &clock_ms, "ran"));
    int after = pool_thread_count();
    if (after != 1) {
        fprintf(stderr, "%d pool threads after one off-thread job, want 1\n", after);
        ++failures;
    }

    /* Idle: the pool thread collects its heap after 1 s and exits 2 s later (wall time, not the host
     * clock). Polls for up to 10 s. */
    int idle = -1;
    for (int i = 0; i < 100; ++i) {
        idle = pool_thread_count();
        if (idle == 0)
            break;
        usleep(100 * 1000);
    }
    if (idle != 0) {
        fprintf(stderr, "%d pool threads after 10 s idle, want 0\n", idle);
        ++failures;
    }

    /* The next off-thread job starts a thread again. */
    CHECK(load(view, "<!doctype html><title>y</title><script src=\"data:text/javascript,document.title%3D%22again%22\"></script>"));
    CHECK(settle_title(view, &clock_ms, "again"));
    int respawned = pool_thread_count();
    if (respawned != 1) {
        fprintf(stderr, "%d pool threads after a job on a retired pool, want 1\n", respawned);
        ++failures;
    }

    kr_view_destroy(view);
    kr_system_destroy(system);
    if (failures) {
        fprintf(stderr, "karouka_pool_threads_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_pool_threads_test: ok\n");
    return 0;
}
