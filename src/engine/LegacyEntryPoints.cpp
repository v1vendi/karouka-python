/*
 * The entry points that hosts built against the headers before struct_size call.
 * Not declared in karouka.h. Each takes its struct in the layout those headers had and forwards to
 * the sized entry point, reading or writing only the bytes every one of those headers' structs
 * held: the structs grew from header to header with nothing in them saying which one a host has.
 */

#include "HostInterface.h"

#include <karouka/karouka.h>

// kr_system_desc's sizes before struct_size were 16, 24, 32, 48 and 56 bytes (gpu_cache_bytes,
// host, mtl_device and mtl_queue, real_time_clock added in turn); only the first header's two
// members are in all of them.
struct kr_system_desc_v0 {
    char const* resource_root;
    bool verbose;
};
static_assert(sizeof(kr_system_desc_v0) == 16);

// kr_view_desc was these 16 bytes in every header before struct_size.
struct kr_view_desc_v0 {
    uint32_t width;
    uint32_t height;
    float scale;
    bool transparent_background;
};
static_assert(sizeof(kr_view_desc_v0) == 16);

// kr_view_stats before struct_size: these 48 bytes, and Now layout_updates after them.
struct kr_view_stats_v0 {
    uint64_t frames_rendered;
    uint64_t updates_run;
    double last_update_ms;
    double last_record_ms;
    double last_raster_ms;
    uint64_t malloc_bytes;
};
static_assert(sizeof(kr_view_stats_v0) == 48);

// kr_memory_stats before struct_size: 48 bytes in the first header, 56 since gc_heap_bytes;
// these are the first 48 of the later layout.
struct kr_memory_stats_v0 {
    uint64_t footprint_bytes;
    uint64_t malloc_zone_bytes;
    uint64_t kmalloc_bytes;
    uint64_t gc_heap_bytes;
    uint64_t gpu_cache_bytes;
    uint64_t gpu_cache_budget_bytes;
};
static_assert(sizeof(kr_memory_stats_v0) == 48);

// kr_font_desc, kr_input_event, kr_metal_output and kr_engine_callbacks
// each had one layout before struct_size: these, read whole.
struct kr_font_desc_v0 {
    char const* family;
    uint32_t collection_index;
    uint32_t flags;
};
static_assert(sizeof(kr_font_desc_v0) == 16);

struct kr_input_event_v0 {
    kr_input_type type;
    float x, y;
    float delta_x, delta_y;
    uint32_t button;
    uint32_t modifiers;
    uint32_t key_code;
    uint32_t code_point;
};
static_assert(sizeof(kr_input_event_v0) == 36);

struct kr_metal_output_v0 {
    void* command_buffer;
    void* texture;
};
static_assert(sizeof(kr_metal_output_v0) == 16);

struct kr_engine_callbacks_v0 {
    void (*trigger)(void* user_data, kr_view* view, char const* name, char const* json_args);
    void (*call)(void* user_data, kr_view* view, char const* name, char const* json_args, uint64_t call_id);
    void* user_data;
};
static_assert(sizeof(kr_engine_callbacks_v0) == 24);

// NULL stays NULL (each function's default); otherwise the old members in the current layout.
static kr_font_desc const* font_desc_from(kr_font_desc_v0 const* old_desc, kr_font_desc& desc)
{
    if (!old_desc)
        return nullptr;
    desc = { .struct_size = sizeof(desc), .family = old_desc->family, .collection_index = old_desc->collection_index, .flags = old_desc->flags };
    return &desc;
}

extern "C" {

KR_API kr_result kr_system_create(kr_system_desc_v0 const* old_desc, kr_system** out_system);
KR_API kr_result kr_view_create(kr_system* system, kr_view_desc_v0 const* old_desc, kr_view** out_view);
KR_API kr_result kr_view_get_stats(kr_view* view, kr_view_stats_v0* out_stats);
KR_API kr_result kr_system_get_memory_stats(kr_system* system, kr_memory_stats_v0* out_stats);
KR_API kr_result kr_system_add_font(kr_system* system, void const* data, size_t size, kr_font_desc_v0 const* old_desc);
KR_API kr_result kr_system_add_font_file(kr_system* system, char const* path, kr_font_desc_v0 const* old_desc);
KR_API kr_result kr_view_inject_input(kr_view* view, kr_input_event_v0 const* old_event);
KR_API kr_result kr_view_set_output(kr_view* view, kr_metal_output_v0 const* old_output);
KR_API void kr_view_set_engine_callbacks(kr_view* view, kr_engine_callbacks_v0 const* old_callbacks);

kr_result kr_system_create(kr_system_desc_v0 const* old_desc, kr_system** out_system)
{
    kr_system_desc desc {};
    desc.struct_size = sizeof(desc);
    if (old_desc) {
        desc.resource_root = old_desc->resource_root;
        desc.verbose = old_desc->verbose;
    }
    auto result = kr_system_create2(&desc, out_system);
    if (result == KR_OK)
        Karouka::log(KR_LOG_WARN, "kr_system_create: the host was built against a karouka.h without struct_size; only resource_root and verbose were read, and gpu_cache_bytes, host, mtl_device, mtl_queue and real_time_clock are their defaults. Rebuild it against the current header (kr_system_create2).");
    return result;
}

kr_result kr_view_create(kr_system* system, kr_view_desc_v0 const* old_desc, kr_view** out_view)
{
    if (!old_desc)
        return KR_ERROR_INVALID_ARGUMENT;
    kr_view_desc desc {};
    desc.struct_size = sizeof(desc);
    desc.width = old_desc->width;
    desc.height = old_desc->height;
    desc.scale = old_desc->scale;
    desc.transparent_background = old_desc->transparent_background;
    return kr_view_create2(system, &desc, out_view);
}

kr_result kr_view_get_stats(kr_view* view, kr_view_stats_v0* out_stats)
{
    if (!out_stats)
        return KR_ERROR_INVALID_ARGUMENT;
    kr_view_stats stats {};
    stats.struct_size = sizeof(stats);
    auto result = kr_view_query_stats(view, &stats);
    if (result != KR_OK)
        return result;
    *out_stats = {
        .frames_rendered = stats.frames_rendered,
        .updates_run = stats.updates_run,
        .last_update_ms = stats.last_update_ms,
        .last_record_ms = stats.last_record_ms,
        .last_raster_ms = stats.last_raster_ms,
        .malloc_bytes = stats.malloc_bytes,
    };
    return KR_OK;
}

kr_result kr_system_get_memory_stats(kr_system* system, kr_memory_stats_v0* out_stats)
{
    if (!out_stats)
        return KR_ERROR_INVALID_ARGUMENT;
    kr_memory_stats stats {};
    stats.struct_size = sizeof(stats);
    auto result = kr_system_query_memory_stats(system, &stats);
    if (result != KR_OK)
        return result;
    *out_stats = {
        .footprint_bytes = stats.footprint_bytes,
        .malloc_zone_bytes = stats.malloc_zone_bytes,
        .kmalloc_bytes = stats.kmalloc_bytes,
        .gc_heap_bytes = stats.gc_heap_bytes,
        .gpu_cache_bytes = stats.gpu_cache_bytes,
        .gpu_cache_budget_bytes = stats.gpu_cache_budget_bytes,
    };
    return KR_OK;
}

kr_result kr_system_add_font(kr_system* system, void const* data, size_t size, kr_font_desc_v0 const* old_desc)
{
    kr_font_desc desc;
    return kr_system_add_font2(system, data, size, font_desc_from(old_desc, desc));
}

kr_result kr_system_add_font_file(kr_system* system, char const* path, kr_font_desc_v0 const* old_desc)
{
    kr_font_desc desc;
    return kr_system_add_font_file2(system, path, font_desc_from(old_desc, desc));
}

kr_result kr_view_inject_input(kr_view* view, kr_input_event_v0 const* old_event)
{
    if (!old_event)
        return KR_ERROR_INVALID_ARGUMENT;
    kr_input_event event {
        .struct_size = sizeof(event),
        .type = old_event->type,
        .x = old_event->x,
        .y = old_event->y,
        .delta_x = old_event->delta_x,
        .delta_y = old_event->delta_y,
        .button = old_event->button,
        .modifiers = old_event->modifiers,
        .key_code = old_event->key_code,
        .code_point = old_event->code_point,
    };
    return kr_view_inject_input2(view, &event);
}

kr_result kr_view_set_output(kr_view* view, kr_metal_output_v0 const* old_output)
{
    if (!old_output)
        return kr_view_set_output2(view, nullptr);
    kr_metal_output output { .struct_size = sizeof(output), .command_buffer = old_output->command_buffer, .texture = old_output->texture };
    return kr_view_set_output2(view, &output);
}

void kr_view_set_engine_callbacks(kr_view* view, kr_engine_callbacks_v0 const* old_callbacks)
{
    if (!old_callbacks) {
        kr_view_set_engine_callbacks2(view, nullptr);
        return;
    }
    kr_engine_callbacks callbacks { .struct_size = sizeof(callbacks), .trigger = old_callbacks->trigger, .call = old_callbacks->call, .user_data = old_callbacks->user_data };
    kr_view_set_engine_callbacks2(view, &callbacks);
}

}
