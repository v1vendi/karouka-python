/* kr_system_desc.shader_cache_path: Skia's programs persist across processes. One system
 * per process, so the test runs itself three times as a child, each rendering the same page with
 * the same cache file: over a file of garbage (ignored, then replaced), then twice over the file
 * the run before wrote. Each child reports the cache's hits, misses and precompiled programs from
 * karouka's `shader cache: H hits, M misses, P precompiled, N programs` line (KR_DEBUG=1, through
 * the host log). The first run must build programs (misses) and write a file; the next must find
 * all of them, those of the first frame precompiled from the file's warm list at kr_system_create2
 * and the rest loaded (hits), and leave the file as it was. Built as karouka_shader_cache_test (C, public header only). */

#include <karouka/karouka.h>

#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

/* Rounded corners, a gradient, a shadow, text and an opacity layer: several distinct programs. */
static char const page[] =
    "<!doctype html><body style='margin:0;font:16px sans-serif'>"
    "<div style='width:80px;height:40px;border-radius:12px;background:linear-gradient(red,blue);box-shadow:0 4px 8px black'></div>"
    "<p style='opacity:.5;transform:rotate(3deg)'>shader cache</p>"
    "</body>";

static long hits = -1, misses = -1, precompiled = -1, programs = -1;

static void write_log(void* user_data, kr_log_level level, char const* category, char const* message)
{
    (void)user_data;
    (void)level;
    if (strcmp(category, "karouka") == 0)
        sscanf(message, "shader cache: %ld hits, %ld misses, %ld precompiled, %ld programs", &hits, &misses, &precompiled, &programs);
}

/* One run: render the page with the cache at `path`, then print "hits misses precompiled programs". */
static int child(char const* path)
{
    kr_host_interface host = { .struct_size = sizeof(kr_host_interface) };
    host.log.write = write_log;
    host.log.min_level = KR_LOG_DEBUG;
    kr_system_desc system_desc = { .struct_size = sizeof(kr_system_desc), .host = &host, .shader_cache_path = path };
    kr_system* system = NULL;
    if (kr_system_create2(&system_desc, &system) != KR_OK) {
        fprintf(stderr, "karouka_shader_cache_test: engine failed to start\n");
        return 1;
    }
    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc), .width = 128, .height = 96, .scale = 1 };
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    CHECK(kr_view_load_html(view, page, strlen(page), "file:///karouka-shader-cache-test/") == KR_OK);
    double clock_ms = 0;
    int rendered_frames = 0;
    for (int frame = 0; frame < 5; ++frame) {
        clock_ms += 16.0;
        bool rendered = false;
        CHECK(kr_view_advance_time(view, clock_ms) == KR_OK);
        CHECK(kr_view_render(view, &rendered) == KR_OK);
        rendered_frames += rendered;
    }
    CHECK(rendered_frames >= 1);
    kr_view_destroy(view);
    kr_system_destroy(system);
    printf("%ld %ld %ld %ld\n", hits, misses, precompiled, programs);
    return failures ? 1 : 0;
}

/* Runs `self` as a child over `path`; reads its hits, misses, precompiled and programs. */
static int run_child(char* self, char* path, long out[4])
{
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    setenv("KR_DEBUG", "1", 1);
    char child_flag[] = "--child";
    char* argv[] = { self, child_flag, path, NULL };
    pid_t pid;
    int spawned = posix_spawn(&pid, self, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (spawned != 0) {
        close(pipe_fds[0]);
        return -1;
    }
    char line[128] = { 0 };
    FILE* output = fdopen(pipe_fds[0], "r");
    if (!fgets(line, sizeof line, output))
        line[0] = 0;
    fclose(output);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return sscanf(line, "%ld %ld %ld %ld", &out[0], &out[1], &out[2], &out[3]) == 4 ? 0 : -1;
}

static long file_size(char const* path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "--child") == 0)
        return child(argv[2]);

    char directory[] = "/tmp/karouka-shader-cache-test-XXXXXX";
    if (!mkdtemp(directory)) {
        perror("mkdtemp");
        return 1;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/cache/shaders.bin", directory);
    char cache_directory[256];
    snprintf(cache_directory, sizeof cache_directory, "%s/cache", directory);
    CHECK(mkdir(cache_directory, 0755) == 0);
    FILE* garbage = fopen(path, "wb");
    CHECK(garbage != NULL);
    if (garbage) {
        fputs("KRSC not a shader cache", garbage);
        fclose(garbage);
    }

    long first[4] = { -1, -1, -1, -1 }, second[4] = { -1, -1, -1, -1 }, third[4] = { -1, -1, -1, -1 };
    CHECK(run_child(argv[0], path, first) == 0);
    long size_after_first = file_size(path);
    CHECK(run_child(argv[0], path, second) == 0);
    long size_after_second = file_size(path);
    CHECK(run_child(argv[0], path, third) == 0);

    /* The garbage was ignored: every program was built, and the file now holds them. */
    CHECK(first[0] == 0 && first[1] > 0 && first[2] == 0 && first[3] == first[1]);
    CHECK(size_after_first > 64);
    /* The next runs build nothing and write nothing new; the first frame's programs were precompiled. */
    CHECK(second[1] == 0 && second[2] > 0 && second[0] + second[2] == first[3] && second[3] == first[3]);
    /* How many of the later frames' programs the GPU thread reaches before kr_system_create2 joins
     * it varies, so only the sum of precompiled and loaded programs is the same. */
    CHECK(third[1] == 0 && third[2] > 0 && third[0] + third[2] == first[3] && third[3] == first[3]);
    CHECK(size_after_second == size_after_first);

    if (failures)
        fprintf(stderr, "  hits/misses/precompiled/programs: first %ld/%ld/%ld/%ld second %ld/%ld/%ld/%ld third %ld/%ld/%ld/%ld; file %ld then %ld bytes\n",
            first[0], first[1], first[2], first[3], second[0], second[1], second[2], second[3], third[0], third[1], third[2], third[3], size_after_first, size_after_second);
    unlink(path);
    rmdir(cache_directory);
    rmdir(directory);
    if (failures)
        fprintf(stderr, "karouka_shader_cache_test: %d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
