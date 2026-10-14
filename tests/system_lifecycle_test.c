/* One system per process: a second kr_system_create2 while a system is alive fails with
 * KR_ERROR_INVALID_ARGUMENT, and a create after kr_system_destroy fails with KR_ERROR_INVALID_STATE
 * instead of running LibWeb's process-wide setup again on halted threads. The process then exits
 * cleanly. Built as karouka_system_lifecycle_test (C, public header only) and run by
 * tools/dev.sh check. */

#include <karouka/karouka.h>

#include <stdio.h>

static int failures;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

int main(void)
{
    kr_system_desc desc = { .struct_size = sizeof(kr_system_desc) };
    kr_system* system = NULL;
    if (kr_system_create2(&desc, &system) != KR_OK || !system) {
        fprintf(stderr, "karouka_system_lifecycle_test: engine failed to start\n");
        return 1;
    }

    /* A second system while the first is alive. */
    kr_system* second = NULL;
    CHECK(kr_system_create2(&desc, &second) == KR_ERROR_INVALID_ARGUMENT);
    CHECK(second == NULL);

    kr_view_desc view_desc = { .struct_size = sizeof(kr_view_desc) };
    view_desc.width = 32;
    view_desc.height = 32;
    view_desc.scale = 1;
    kr_view* view = NULL;
    CHECK(kr_view_create2(system, &view_desc, &view) == KR_OK);
    kr_system_destroy(system); /* destroys the view too */

    /* After destroy: no system can be created again, with or without a desc, every time. */
    kr_system* again = NULL;
    CHECK(kr_system_create2(&desc, &again) == KR_ERROR_INVALID_STATE);
    CHECK(again == NULL);
    CHECK(kr_system_create2(NULL, &again) == KR_ERROR_INVALID_STATE);
    CHECK(again == NULL);
    /* A NULL out pointer is still an argument error, checked first. */
    CHECK(kr_system_create2(&desc, NULL) == KR_ERROR_INVALID_ARGUMENT);

    if (failures) {
        fprintf(stderr, "karouka_system_lifecycle_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("karouka_system_lifecycle_test: ok\n");
    return 0;
}
