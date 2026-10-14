/*
 * karouka: render an HTML/CSS/JS page into a GPU texture, in-process, with Ladybird's engine.
 *
 * The frame loop mirrors Tessera's (tsr_view_*) so hosts and harnesses can drive both the same way:
 *
 *     kr_view_inject_input2(...)  zero or more times
 *     kr_view_advance_time(...)   timers, rAF, animations, style, layout
 *     kr_view_render(...)         record + rasterize into the view's output (only when something changed)
 *
 * The output is an IOSurface (BGRA8, premultiplied) the host wraps as its own MTLTexture, or, with the
 * host's Metal device and queue (kr_system_desc.mtl_device), the host's own texture, drawn into
 * directly (kr_view_set_output2); the view then has no IOSurface. With the software backend
 * (kr_system_desc.backend = KR_BACKEND_SOFTWARE) karouka uses no GPU: Skia's CPU rasterizer writes
 * each frame into the IOSurface's memory.
 * Everything runs on the thread that created the system. There is no networking: only file:,
 * data: and about: URLs load.
 *
 * A system can have several views. They share one script VM, one event loop and one rendering
 * update, which serves every view's page at once, so with several views a host frame is best:
 *
 *     kr_view_inject_input2(view_i, ...)     for each view
 *     kr_view_advance_time(view_i, now_ms)   for each view, with the same now_ms
 *     kr_view_render(view_i, ...)            for each view
 *
 * The first advance at a given now_ms runs the update for all views; the others at that now_ms
 * only run due tasks. Each view still renders only when its own page changed.
 *
 * Views share the system's one GPU context (Metal device, command queue, Skia context and its
 * resource cache, see kr_system_desc.gpu_cache_bytes); a view's own GPU memory is its IOSurface,
 * none when it draws into the host's textures.
 * The device and queue are the host's when it passes them (kr_system_desc.mtl_device/mtl_queue).
 */
#ifndef KAROUKA_H
#define KAROUKA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(KAROUKA_BUILDING)
#    define KR_API __attribute__((visibility("default")))
#else
#    define KR_API
#endif

typedef struct kr_system kr_system;
typedef struct kr_view kr_view;

typedef enum kr_result {
    KR_OK = 0,
    KR_ERROR_INVALID_ARGUMENT = 1,
    KR_ERROR_IO = 2,
    KR_ERROR_GPU = 3,
    KR_ERROR_INTERNAL = 4,
    KR_ERROR_LIMIT_EXCEEDED = 5, /* a fixed ceiling was reached; nothing changed */
    KR_ERROR_INVALID_STATE = 6,  /* the call conflicts with the object's state; nothing changed */
} kr_result;

/*
 * Host interface: the seams through which karouka reaches the outside world, after Tessera's
 * tsr_host_interface. Every member is optional, and a
 * zeroed member means karouka's own default. Ladybird is not syscall-free, so these are the
 * embedder-visible seams only; what still bypasses each one is listed with it.
 *
 * There is no allocator member. Nothing karouka allocates through can be redirected to a host
 * allocator without patching Ladybird: AK's kmalloc (LibJS/LibWeb objects, strings, buffers) is
 * mimalloc, the GC heap is LibGC's own reserved regions, and Skia, CoreText, Metal and the
 * Objective-C runtime use the system malloc zones. kr_system_get_memory_stats reports the totals.
 */

/* ---- io: file loads */

typedef enum kr_io_ownership {
    /* karouka copies the bytes before kr_io_complete returns; the host may free them at once. */
    KR_IO_OWNERSHIP_COPY = 0,
    /* Same copy, then karouka calls kr_host_io.release(id, data) before kr_io_complete returns. */
    KR_IO_OWNERSHIP_BORROW_UNTIL_RELEASE = 1,
} kr_io_ownership;

/* Routes file: loads through the host, so pages can come from pak files or a virtual file system:
 * kr_view_load_file, link navigation (which loads through kr_view_load_file), and every file:
 * subresource LibWeb's ResourceLoader loads for a page (stylesheets, scripts, images, fonts, frames).
 * (fetch() of a file: URL is refused before it loads: file: origins are opaque.)
 *
 * request receives an absolute file: URL (percent-encoded, NUL-terminated) and an id, and answers
 * with kr_io_complete(system, id, ...), either from inside request or later, on the thread that
 * drives the system, while no other karouka call is in flight. Returning anything but KR_OK from
 * request without completing fails the load with that status. A completion from inside request is
 * acted on at once; a later one in a task the next kr_view_advance_time runs. cancel (optional) is
 * called for a load karouka no longer wants (its view was destroyed); the host may still complete
 * it, and the completion is ignored. release is required only for BORROW_UNTIL_RELEASE.
 *
 * Not routed: resource: URLs (Ladybird's own stylesheets and icons, read through Core::Resource
 * with mmap), the system UI font (CoreText opens it), and file: subresources in documents of worker or detached contexts
 * that have no page. A file: URL naming a directory gets no listing through the host. */
typedef struct kr_host_io {
    kr_result (*request)(void* user_data, char const* url, uint64_t request_id);
    void (*cancel)(void* user_data, uint64_t request_id);
    void (*release)(void* user_data, uint64_t request_id, void const* data);
    void* user_data;
} kr_host_io;

/* Delivers a load requested through kr_host_io.request. status KR_OK with the bytes, or the failure
 * (KR_ERROR_IO for a missing file); data may be NULL when size is 0.
 * KR_ERROR_INVALID_ARGUMENT for a NULL system, an id that is not pending, or NULL data with a
 * nonzero size; an error return copies and releases nothing. */
KR_API kr_result kr_io_complete(kr_system* system, uint64_t request_id, kr_result status, void const* data, size_t size,
    kr_io_ownership ownership);

/* ---- clock */

/* Optional. The monotonic clock karouka reads for its own timings: kr_view_stats' update, record
 * and raster times and the idle-trim bookkeeping. NULL: the system's monotonic clock.
 * The page's clock (timers, performance.now(), rAF, animations) is not this callback: it is the
 * now_ms the host passes to kr_view_advance_time, or with kr_system_desc.real_time_clock the
 * system's monotonic clock. */
typedef struct kr_host_clock {
    uint64_t (*monotonic_ns)(void* user_data);
    void* user_data;
} kr_host_clock;

/* ---- log */

typedef enum kr_log_level {
    KR_LOG_TRACE = 0,
    KR_LOG_DEBUG = 1,
    KR_LOG_INFO = 2,
    KR_LOG_WARN = 3,
    KR_LOG_ERROR = 4,
} kr_log_level;

/* Optional. With write set, karouka delivers to it, on the thread driving the system:
 *   category "console": the console.* output of each view's top-level document (log/info as INFO,
 *     debug and trace as DEBUG, warn as WARN, error and failed console.assert as ERROR) and its
 *     uncaught exceptions and unhandled promise rejections (ERROR);
 *   category "karouka": karouka's own diagnostics (refused navigations, binding attribute errors;
 *     with KR_DEBUG=1 its per-frame lines at DEBUG).
 * min_level is a floor karouka enforces: a line below it is never formatted. Strings are
 * NUL-terminated UTF-8, borrowed until write returns. Without write, the "karouka" lines at INFO
 * and above go to stderr (DEBUG and TRACE too with KR_DEBUG=1), and the "console" lines go to
 * stderr only when kr_system_desc.verbose is set.
 * Not routed: Ladybird's own dbgln output (AK's vdbg writes straight to stderr: load failures,
 * parser and CSS warnings, and a second copy of each uncaught exception), and console output of
 * workers. */
typedef struct kr_host_log {
    void (*write)(void* user_data, kr_log_level level, char const* category, char const* message);
    kr_log_level min_level;
    void* user_data;
} kr_host_log;

typedef struct kr_host_interface {
    /* sizeof(kr_host_interface); members that don't end within it read as zero. One too small to
     * hold io (0: a struct the host zeroed and didn't size) is refused by kr_system_create2. */
    uint32_t struct_size;
    kr_host_io io;
    kr_host_clock clock;
    kr_host_log log;
} kr_host_interface;

/* The structs the host hands in (kr_system_desc, kr_view_desc, kr_font_desc, kr_input_event,
 * kr_metal_output, kr_engine_callbacks) and the stats structs karouka fills (kr_view_stats,
 * kr_memory_stats, kr_view_dependencies) are versioned by a leading struct_size, Tessera's
 * convention: the host sets it to sizeof the struct as the host was compiled. Members
 * are only ever added at the end. karouka reads an input struct's members that end within
 * struct_size and takes the ones past it (a member a newer header added) as zero, its default; it
 * fills a stats struct's members that end within struct_size and writes nothing past it. A
 * struct_size too small to hold the first member after it (0: a struct the host zeroed and didn't
 * size) is refused with KR_ERROR_INVALID_ARGUMENT.
 *
 * Before struct_size these structs had no such member, and the
 * functions that take them were kr_system_create, kr_view_create, kr_system_get_memory_stats,
 * kr_view_get_stats, kr_system_add_font, kr_system_add_font_file, kr_view_inject_input,
 * kr_view_set_output and kr_view_set_engine_callbacks. libkarouka still exports those symbols for
 * hosts built against those headers, in their old layouts and limited to the members every one of
 * those headers had (each is noted below); they are not declared here. */

/* How karouka rasterizes (kr_system_desc.backend). Tessera's backends are software, GL and Metal
 *; karouka has Skia's Metal backend and Skia's CPU rasterizer. */
typedef enum kr_backend {
    /* Skia's Metal backend, on the host's device and queue or on ones karouka makes (the default). */
    KR_BACKEND_METAL = 0,
    /* Skia's CPU rasterizer: no Metal device, command queue or GPU resource cache is created, so a
     * machine without a GPU (a CI runner, a VM) can render. Each view still renders into its
     * IOSurface (BGRA8, premultiplied), on the CPU of the thread that calls kr_view_render, which
     * returns with the frame in the IOSurface's memory. mtl_device and mtl_queue must be NULL
     * (KR_ERROR_INVALID_ARGUMENT otherwise), gpu_cache_bytes is ignored, kr_view_set_output2 is
     * refused (KR_ERROR_INVALID_STATE), and the GPU cache counters stay 0. Pixels may differ slightly from
     * the Metal backend's: anti-aliasing of paths and edges is rasterized differently. */
    KR_BACKEND_SOFTWARE = 1,
} kr_backend;

typedef struct kr_system_desc {
    uint32_t struct_size; /* sizeof(kr_system_desc) */
    /* Directory holding Ladybird's resources (fonts, default stylesheets). NULL: $KR_RESOURCE_ROOT if
     * set and not empty, else the build's default. */
    char const* resource_root;
    /* Without a host log callback, print the pages' console output to stderr. */
    bool verbose;
    /* Budget, in bytes, of the GPU resource cache (glyph atlases, gradients, layer textures, image
     * uploads) that every view of the system shares. 0: 64 MiB. */
    size_t gpu_cache_bytes;
    /* Optional host callbacks, copied (the struct need not outlive the call). NULL: all defaults. */
    kr_host_interface const* host;
    /* The host's GPU: an id<MTLDevice> and an id<MTLCommandQueue> of that device, both or neither
     * (KR_ERROR_INVALID_ARGUMENT otherwise). karouka retains them until kr_system_destroy, draws
     * every view with that device and submits its command buffers to that queue, so Metal runs
     * them in order with the command buffers the host commits to the queue after them.
     * kr_view_render then does not wait for the GPU, and kr_view_set_output2 can have it draw into
     * the host's textures. NULL: karouka picks a GPU, makes its own queue, and kr_view_render waits
     * for the GPU before it returns. */
    void* mtl_device;
    void* mtl_queue;
    /* false (the default): the engine's clock is the host's (see kr_view_advance_time). It moves
     * only when the host advances it, so a host that steps it by a fixed amount gets the same
     * frames on every run, but it stands still while a task runs: a script that waits for
     * performance.now() to move (a busy-wait) never returns.
     * true: the engine's clock is the system's monotonic clock, as in a browser. performance.now()
     * moves while a script runs, and timers fire when real time reaches their deadline, at the
     * kr_view_advance_time after it; now_ms then only names the frame (a frame time ahead of the
     * real clock is clamped to it), and frames depend on what they cost. kr_host_clock is not read
     * for it either. */
    bool real_time_clock;
    /* A kr_backend: KR_BACKEND_METAL (0, the default) or KR_BACKEND_SOFTWARE; any other value is
     * KR_ERROR_INVALID_ARGUMENT. 64 bits wide so that it starts past the end of the struct before it
     * (64 bytes): a 32-bit member would sit in that struct's tail padding, which a host built against
     * the older header, setting struct_size to 64, may have left uninitialized. */
    uint64_t backend;
    /* A file in which Skia keeps the GPU programs it builds (UTF-8 path; NULL or "": none, the
     * default). Skia's Metal backend builds a program for each new kind of draw it meets, in the
     * kr_view_render that first draws it: a page's first frame builds a dozen or so, several
     * milliseconds of CPU in that render. kr_system_create2 reads the file, and the renders take
     * the programs they find there instead of generating and compiling them; kr_system_destroy writes
     * it back when the run built programs the file lacked, or when the programs views' first
     * frames used changed (into a temporary file renamed over it; the directory holding it is
     * created if missing, but not its parents). kr_system_create2 precompiles the programs the
     * last run's first frames used (up to 24), off the calling thread, so that a view's first
     * kr_view_render finds them built. A file written by another version of Skia or for another
     * GPU is ignored and replaced. Next to it, "<path>.glyphs" lists the glyphs views' first frames
     * drew in web fonts, and those their display lists held offscreen; when a page loads one of those
     * fonts again, they are rasterized on a worker thread, the drawn ones before the render that
     * first shows them and the offscreen ones between later renders (KR_WEBFONT_GLYPHS=0 turns that
     * off).
     * Ignored with KR_BACKEND_SOFTWARE. */
    char const* shader_cache_path;
} kr_system_desc;

/* One system per process: LibWeb's VM and plugins are process-wide. A second create while a system
 * is alive fails with KR_ERROR_INVALID_ARGUMENT; a create after kr_system_destroy, or after a create
 * that failed with KR_ERROR_GPU, fails with KR_ERROR_INVALID_STATE (LibWeb's setup can't run twice).
 *
 * Fonts: the only system font karouka uses is the macOS system UI font (San Francisco), upright and
 * italic, every weight through its variation axes. Every generic family (serif, sans-serif,
 * monospace, cursive, fantasy, system-ui, ui-serif, ui-monospace, ui-rounded, ui-sans-serif) and
 * every family a page names without defining it with @font-face renders in SF. There is no system glyph fallback: text SF lacks (CJK, Arabic, emoji and
 * other scripts) renders as SF's missing-glyph box unless the page ships a font for it with
 * @font-face or the host registers one (kr_system_add_font2, below). No font directory is scanned. */
/* desc may be NULL: every member's default. KR_ERROR_INVALID_ARGUMENT for a desc whose struct_size
 * can't hold resource_root, or a desc->host whose struct_size can't hold io (see struct_size above).
 *
 * The legacy kr_system_create(desc, out_system), for hosts built before struct_size, reads only
 * resource_root and verbose (the members of the first header): a desc from those headers may end
 * after any later member, so gpu_cache_bytes, host, mtl_device, mtl_queue and real_time_clock take
 * their defaults, and it logs a warning that says so. */
KR_API kr_result kr_system_create2(kr_system_desc const* desc, kr_system** out_system);
/* Destroys any views still alive first (their handles become invalid).
 *
 * Call it before the process exits (exit(), or returning from main), on the thread that drives the
 * system. LibWeb runs layout, paint and off-thread work (script compiles, CSS parsing, font loads)
 * on threads that live as long as the process. kr_system_destroy waits for the work already handed
 * to them and then stops them for good. A process that exits with the system alive runs its static
 * destructors (Skia's caches among them) while those threads may still be using them, and can
 * crash on the way out. A process that ends with _exit() runs no destructors and has no such race.
 * No other system can be created after it (kr_system_create2 returns KR_ERROR_INVALID_STATE):
 * LibWeb's process-wide state can't be set up twice, and its threads stay stopped. */
KR_API void kr_system_destroy(kr_system* system);

/* ---- fonts: the host's own font faces (for fonts shipped in pak files, or a last-resort font)
 *
 * Fonts are per system, not per view: LibGfx's font database, and the glyph-fallback memo every
 * document shares, are process-wide, and there is one system per process. A page that wants a font
 * only for itself ships it with @font-face (which kr_host_io routes like any other file: load).
 *
 * A registered face answers to its family name ahead of SF in every view: font-family: <family>
 * selects it, the face nearest the requested weight, width and slope when the family has several
 * (register each face of the family under the same name). A family defined with @font-face by the
 * page wins over a registered family of the same name. local("<full or PostScript name>") in an
 * @font-face src finds a registered face as well. Weight and slope come from the font's own tables
 * (OS/2, head); there is no override, because LibWeb's font matching reads them from the face.
 *
 * A face registered after a page loaded reaches it: the elements naming its family are styled and
 * laid out again (and every page's text after a KR_FONT_FALLBACK face), on the next frame. One
 * thing does not reach a loaded page: a local() source that found nothing when the page's
 * @font-face rule loaded. Registered faces stay until the process exits (they cannot be
 * removed). Formats: TrueType, OpenType and collections (.ttf, .otf, .ttc), WOFF and WOFF2.
 * Thread: the one that drives the system, like every other call; the faces are then read under a
 * lock by LibWeb's layout threads too.
 *
 * Results: KR_ERROR_INVALID_ARGUMENT for a NULL or destroyed system, NULL or empty data or path, an empty
 * family, a collection index past the file's faces, or bytes that are no font; KR_ERROR_IO for a
 * file that cannot be read. On an error nothing is registered. */

typedef enum kr_font_flags {
    KR_FONT_FLAGS_NONE = 0,
    /* Also a glyph fallback: a code point that no font of the element's cascade covers (its
     * font-family list, then SF) renders with the first fallback face, in registration order, that
     * covers it, instead of SF's missing-glyph box. This is the host's last-resort font for scripts
     * SF lacks (CJK, Arabic, emoji). */
    KR_FONT_FALLBACK = 1u << 0,
} kr_font_flags;

typedef struct kr_font_desc {
    uint32_t struct_size; /* sizeof(kr_font_desc) */
    /* The family name the face answers to. NULL: the font's own family name (its name table). */
    char const* family;
    /* The face to use in a collection (.ttc/.otc); 0 otherwise. */
    uint32_t collection_index;
    uint32_t flags; /* kr_font_flags */
} kr_font_desc;

/* Registers a face from memory. The bytes are copied before the call returns (LibGfx keeps font
 * data only in its own buffers), so the host may free them at once; that is also how bytes a host
 * read through its own io (a pak file) are registered. desc may be NULL: the font's own family name,
 * face 0, no flags. KR_ERROR_INVALID_ARGUMENT for a desc whose struct_size can't hold family.
 *
 * The legacy kr_system_add_font and kr_system_add_font_file (same arguments), for hosts built
 * before struct_size, read the 16 bytes of family, collection_index and flags that header had. */
KR_API kr_result kr_system_add_font2(kr_system* system, void const* data, size_t size, kr_font_desc const* desc);

/* Registers a face from a file. Relative paths resolve against the working directory.
 * With kr_host_io.request set, the file is read through the host like a page: request receives its
 * absolute file: URL. If the host completes inside request, the result is the registration's;
 * otherwise this returns KR_OK at once, the face is registered when the host completes the load,
 * and a failure then is logged (category "karouka", WARN). Without a host io, karouka maps the file
 * (no copy; its pages are shared with the file cache). */
KR_API kr_result kr_system_add_font_file2(kr_system* system, char const* path, kr_font_desc const* desc);

/* Makes a registered family the default font in SF's place: the generic families (serif,
 * sans-serif, monospace, cursive, fantasy, system-ui and the ui-* families), -apple-system, every
 * family name a page uses without defining it, text with no font-family at all, and the cascade's
 * last-resort font for a glyph nothing covers (before that, the KR_FONT_FALLBACK faces are tried).
 * NULL: SF again. It reaches loaded pages too: their text in the families it changes is styled and
 * laid out again on the next frame.
 * KR_ERROR_INVALID_ARGUMENT for a NULL or destroyed system, or a family no registered face answers to. */
KR_API kr_result kr_system_set_default_font(kr_system* system, char const* family);

typedef struct kr_view_desc {
    uint32_t struct_size; /* sizeof(kr_view_desc) */
    uint32_t width;  /* device pixels */
    uint32_t height; /* device pixels */
    float scale;     /* device pixels per CSS pixel; 0 means 1 */
    bool transparent_background;
} kr_view_desc;

/* KR_ERROR_INVALID_ARGUMENT for a NULL system, desc or out_view, a struct_size that can't hold
 * width (see struct_size above), or a zero width or height. The legacy kr_view_create(system, desc,
 * out_view), for hosts built before struct_size, reads the 16 bytes of width, height, scale and
 * transparent_background that every such header had. */
KR_API kr_result kr_view_create2(kr_system* system, kr_view_desc const* desc, kr_view** out_view);
/* Closes the view's page (no unload events) and frees its surface and the surfaces of the page's
 * 2D canvases and OffscreenCanvas placeholders (KR_MEM_TAG_CANVAS). The other views keep running;
 * the page's document is torn down by the next kr_view_advance_time of any view. */
KR_API void kr_view_destroy(kr_view* view);

/* Load a page from disk; relative URLs (stylesheets, scripts, images) resolve against its directory. */
KR_API kr_result kr_view_load_file(kr_view* view, char const* path);
/* Load markup directly; base_url (e.g. "file:///dir/") resolves relative URLs, may be NULL. */
KR_API kr_result kr_view_load_html(kr_view* view, char const* html, size_t length, char const* base_url);

/* Called when the page navigates (link click, location change) to a file: path. Return true to let
 * the view load it, false if the host handles it (for example to keep its own history). */
typedef bool (*kr_navigate_callback)(kr_view* view, char const* path, void* user_data);
KR_API void kr_view_set_navigate_callback(kr_view* view, kr_navigate_callback callback, void* user_data);

/* Embedder script types: the view runs <script type="mime"> through `callback` (docs/PYTHON.md,
 * where the Python module registers text/python this way from C++).
 *
 * Such a script follows HTML's classic-script processing as a JavaScript one would: an inline or
 * src= script without async or defer blocks the parser and runs in document order; defer scripts
 * run in order after parsing, before DOMContentLoaded; async scripts run when loaded, before the
 * window's load event. src= is fetched as a classic script's src= is (a file: URL) and decoded as
 * UTF-8 whatever its charset.
 * document.currentScript is the element while the callback runs. The callback gets the source text
 * (NUL-terminated UTF-8 and its length in bytes), the URL it came from (the document's for an inline
 * script, the file's for src=) and the line the text starts on in it (1 for src=); all are borrowed
 * until it returns. Return true when the script succeeded; false fires `error` at the element
 * instead of `load`. A src= that fails to load fires `error` and never calls it.
 *
 * `mime` is matched by its MIME essence, case-insensitively and without parameters
 * ("text/python" matches type="Text/Python; version=3"). One callback per type and view; a NULL
 * callback removes the type, after which its scripts are ignored, as are those of any type no
 * callback was set for. Applies to scripts prepared from the call on, and lasts across loads.
 * The callback runs synchronously on the thread driving the view (inside kr_view_advance_time or a
 * load), inside the page's realm; microtasks it causes run after it returns. Inside it, do not
 * destroy, load or reload the view.
 * KR_ERROR_INVALID_ARGUMENT, nothing changed: a NULL view or mime, a mime that is not a MIME type,
 * or a JavaScript MIME type (those always run as JavaScript). */
typedef bool (*kr_script_type_callback)(kr_view* view, char const* source, size_t source_length, char const* url,
    uint32_t line, void* user_data);
KR_API kr_result kr_view_set_script_type_callback(kr_view* view, char const* mime, kr_script_type_callback callback,
    void* user_data);

/* Whether the host's window holding the view has focus; a new view is focused. Unfocused, the page
 * paints selections in the inactive selection color, draws no text caret or `outline: auto` ring,
 * and document.hasFocus() is false. A change fires, in a task the next kr_view_advance_time runs,
 * blur (losing focus) or focus (gaining it) at the window and the focused element, which also gets
 * focusout or focusin; the document's focused element stays. The next kr_view_advance_time
 * repaints. Lasts across loads and reloads.
 * KR_ERROR_INVALID_ARGUMENT for a NULL view. */
KR_API kr_result kr_view_set_focused(kr_view* view, bool focused);

/* ---- reload (hot reload, after Tessera's tsr_view_reload) */

/* Loads the view's current page again: the file of the last kr_view_load_file (link navigation
 * included), read again, through kr_host_io when the host routes io; or the markup of the last
 * kr_view_load_html, which the view keeps a copy of for this, with the same base_url. Every
 * subresource is requested again: nothing a document fetched outlives it.
 *
 * Kept: the view (size, scale, surface), its binding model, engine callbacks and navigate callback,
 * and the viewport's scroll position, which is applied to the new document once it has loaded
 * (its load event), clamped to its new size, unless the document scrolled itself first. Not kept:
 * everything of the document (script state, timers, animations, element scroll offsets, focus,
 * form state); the `engine` channel's endpoints start over as on any load.
 *
 * KR_ERROR_INVALID_ARGUMENT for a NULL view; KR_ERROR_INVALID_STATE, nothing changed, when the view
 * has loaded nothing yet. A file that can't be read returns KR_ERROR_IO (or, routed, the host's
 * status) and keeps the current document. With an asynchronous kr_host_io the reload completes when
 * the host does, as kr_view_load_file does. */
KR_API kr_result kr_view_reload(kr_view* view);

/* What the view's current document was loaded from: its own file and every file: URL LibWeb's
 * ResourceLoader asked for on its behalf (stylesheets, scripts and modules, images, fonts, frames),
 * in the order first requested, each once. A host watches these for changes and calls
 * kr_view_reload. A load (kr_view_load_file, kr_view_load_html, link navigation, kr_view_reload)
 * starts a new set, with the document's own entry when it came from a file; a load that fails keeps
 * the old one. Requests made for the document's page by documents of no page (workers) are not
 * seen; fetch() of a file: URL is refused before it loads and is not listed either. */
#define KR_DEPENDENCY_DOCUMENT 1u    /* the page's own file (kr_view_load_file) */
#define KR_DEPENDENCY_SUBRESOURCE 2u /* a file: URL a document of the page requested */
/* Hosts must tolerate kind bits they don't know. */

typedef struct kr_view_dependency {
    char const* url;   /* absolute file: URL, percent-encoded: byte for byte what kr_host_io.request receives */
    uint32_t kinds;    /* KR_DEPENDENCY_* bits: every kind the URL was requested as */
    bool pending;      /* a request for it is still in flight */
    kr_result status;  /* the latest finished request's outcome (KR_OK, or KR_ERROR_IO when it could
                        * not be opened or the host failed it); KR_OK while none has finished */
} kr_view_dependency;

/* Versioned by struct_size, as kr_view_stats: kr_view_get_dependencies writes the members that fit. */
typedef struct kr_view_dependencies {
    uint32_t struct_size;                /* in: sizeof(kr_view_dependencies); left unchanged */
    kr_view_dependency const* entries;   /* view-owned; valid until the next kr_view_get_dependencies
                                          * on this view or kr_view_destroy */
    size_t count;
    uint64_t revision;                   /* changes whenever the entries would read differently,
                                          * across loads too; never 0 */
    bool truncated;                      /* a URL was left out: a set holds at most 4096 */
} kr_view_dependencies;

/* KR_ERROR_INVALID_ARGUMENT, nothing written: a NULL view or out, or a struct_size too small to
 * hold entries. Changes nothing in the view but the array a previous call returned. */
KR_API kr_result kr_view_get_dependencies(kr_view* view, kr_view_dependencies* out);

/* The document's title, UTF-8, "" for a page without one. Valid until the next call. */
KR_API char const* kr_view_title(kr_view* view);

KR_API kr_result kr_view_resize(kr_view* view, uint32_t width, uint32_t height, float scale);

typedef enum kr_input_type {
    KR_INPUT_MOUSE_MOVE,
    KR_INPUT_MOUSE_DOWN,
    KR_INPUT_MOUSE_UP,
    KR_INPUT_WHEEL,
    KR_INPUT_KEY_DOWN,
    KR_INPUT_KEY_UP,
} kr_input_type;

enum {
    KR_MOD_SHIFT = 1 << 0,
    KR_MOD_CTRL = 1 << 1,
    KR_MOD_ALT = 1 << 2,
    KR_MOD_SUPER = 1 << 3,
};

typedef struct kr_input_event {
    uint32_t struct_size; /* sizeof(kr_input_event) */
    kr_input_type type;
    float x, y;          /* device pixels, for mouse/wheel */
    float delta_x, delta_y; /* device pixels, for wheel */
    uint32_t button;     /* 0 primary, 1 secondary, 2 middle */
    uint32_t modifiers;  /* KR_MOD_* */
    uint32_t key_code;   /* macOS virtual key code, for keys */
    uint32_t code_point; /* text the key produces, 0 if none */
} kr_input_event;

/* KR_ERROR_INVALID_ARGUMENT for a NULL view or event, or a struct_size that can't hold type. The
 * legacy kr_view_inject_input(view, event), for hosts built before struct_size, reads the 36 bytes
 * of type through code_point that header had. */
KR_API kr_result kr_view_inject_input2(kr_view* view, kr_input_event const* event);

/* Run due timers, tasks, requestAnimationFrame, animations, style and layout for the given
 * host clock time (milliseconds, monotonic). Timers and tasks of every view of the system run;
 * see the top of this file for several views. The rendering update for this frame has run to its
 * end when this returns (style and layout included), so the next kr_view_render shows exactly this
 * frame, however short the host's frames are.
 * now_ms is the engine's clock: rAF callbacks, CSS animations, performance.now() and timers see
 * the time elapsed on the host's clock since the first advance, and nothing else. It may run ahead
 * of real time, so a host that steps now_ms by a fixed amount gets the same frames on every run.
 * Between the last advance's time and now_ms the clock steps through every whole millisecond, and
 * the timers due at each step fire and their tasks run at that time, so setInterval(f, 10) runs
 * 100 times per second of host time however long the host's frames are (a host jump of more than
 * a second steps more coarsely). Within a task the clock stands still: a script that busy-waits on
 * performance.now() never returns; kr_system_desc.real_time_clock is the mode for such pages.
 * Date.now() still reads the wall clock. An earlier now_ms than the last is ignored.
 * The engine's event loop runs only inside this call, never between calls, and never sleeps or
 * polls: its internal timers (garbage collection, media) fire on the same clock as the page's, and
 * the work other threads hand back (style and layout results, script compiles, a kr_io_complete
 * from later) waits for the next call. */
KR_API kr_result kr_view_advance_time(kr_view* view, double now_ms);

/* Rasterize into the output if anything changed since the last render, or if the output is not the
 * one the last render drew into: the texture kr_view_set_output2 named for this render, else the
 * view's IOSurface. *out_rendered says whether the output's contents changed this call.
 *
 * With the host's queue (kr_system_desc.mtl_queue) this submits the GPU work and returns without
 * waiting for it: the frame is complete for any command buffer the host commits to that queue
 * afterwards, kr_view_set_output2's included. A host that reads the IOSurface on the CPU, or
 * through a texture it made over the IOSurface (Metal does not order that separate resource after
 * karouka's drawing), first waits for such a command buffer to complete. Without the host's queue
 * this waits for the GPU, and the IOSurface holds the frame when it returns. With KR_BACKEND_SOFTWARE
 * the frame is rasterized on the CPU, into the IOSurface, before this returns. */
KR_API kr_result kr_view_render(kr_view* view, bool* out_rendered);

/* The host's texture to draw into, after Tessera's tsr_metal_output
 *. karouka never commits, waits on or presents
 * command_buffer; scheduling stays with the host. */
typedef struct kr_metal_output {
    uint32_t struct_size; /* sizeof(kr_metal_output) */
    /* id<MTLCommandBuffer> made from kr_system_desc.mtl_queue, not yet enqueued or committed (its
     * place in the queue must come after karouka's drawing), with no encoder open. The host
     * commits it after kr_view_render. karouka draws with its own command buffers on that queue,
     * ahead of this one, and encodes nothing into it. */
    void* command_buffer;
    /* id<MTLTexture> of kr_system_desc.mtl_device: 2D, MTLPixelFormatBGRA8Unorm, one sample, with
     * MTLTextureUsageRenderTarget, not framebufferOnly or memoryless. The frame lands at its
     * top-left, clipped to the size both share; pixels outside the frame are left alone. */
    void* texture;
} kr_metal_output;

/* Names the target of the next kr_view_render, which consumes it: set it each frame, as with
 * tsr_metal_backend_set_output. That render draws the view's frame (premultiplied BGRA8, as the
 * IOSurface) straight into output->texture: no IOSurface, no copy. It draws only when the frame
 * changed or the texture is not the one the last render drew into (then the whole frame, also when
 * the page did not change, but not before the view's first frame), so the host must leave the
 * texture's frame alone between renders that name it again, or name another texture. karouka keeps
 * a reference to the texture it last drew into, until a render draws into another target, a resize
 * or kr_view_destroy. A view that renders without a target draws into its IOSurface again (whole).
 * NULL clears a target not yet consumed.
 * KR_ERROR_INVALID_STATE: the system has no host queue (KR_BACKEND_SOFTWARE has none). KR_ERROR_INVALID_ARGUMENT: a NULL view, a
 * struct_size that can't hold command_buffer (or texture: both are required), or an output that
 * breaks a rule above. Nothing changes on an error. The legacy kr_view_set_output(view, output),
 * for hosts built before struct_size, reads the 16 bytes of command_buffer and texture. */
KR_API kr_result kr_view_set_output2(kr_view* view, kr_metal_output const* output);

/* The IOSurfaceRef holding the last rendered frame, or NULL before the first render and while the
 * view draws into the host's textures (kr_view_set_output2). The surface is created by the first
 * kr_view_render without a target at exactly the view's device size (width x height as given),
 * dropped by kr_view_resize to a different size or by a render into a host texture and recreated
 * by the next render without one, and freed by kr_view_destroy. Valid until the next resize, render
 * into a host texture or destroy. See kr_view_render for when
 * its contents are complete.
 *
 * OpenGL (CGL) hosts bind it to a rectangle texture, on every backend (tests/gl_interop_test.c):
 *
 *     glBindTexture(GL_TEXTURE_RECTANGLE, texture);
 *     CGLTexImageIOSurface2D(cgl_context, GL_TEXTURE_RECTANGLE, GL_RGBA, IOSurfaceGetWidth(surface),
 *         IOSurfaceGetHeight(surface), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, surface, 0);
 *
 * GL_TEXTURE_RECTANGLE only (texel coordinates, no mipmaps); the texture aliases the surface's
 * memory, so there is no upload. Bind once per surface: the texture shows each later frame karouka
 * renders into it; bind again when this returns a different pointer (after a resize). Texel row 0
 * is the page's top row, so a host drawing with GL's bottom-left origin flips t. Pixels are
 * premultiplied (blend with GL_ONE, GL_ONE_MINUS_SRC_ALPHA) and sRGB-encoded (GL_RGBA, not
 * GL_SRGB8_ALPHA8, unless the host wants GL to linearize on sampling).
 * Frame-complete point: GL may read the texture once kr_view_render has returned (karouka's own
 * queue, KR_BACKEND_SOFTWARE), or, with the host's queue (kr_system_desc.mtl_queue), once a
 * command buffer the host committed to that queue after kr_view_render has completed (e.g.
 * waitUntilCompleted, or its completion handler). There is one surface, rewritten in place: GL
 * commands that read it must have finished (glFinish, or a glFenceSync waited on) before the next
 * kr_view_render, or they may see part of the next frame. */
KR_API void* kr_view_iosurface(kr_view* view);

/* The last rendered frame's pixels on the CPU, for hosts that upload or encode them themselves
 * (no IOSurface or Metal code): *out_pixels gets the first byte of the frame's top row and
 * *out_bytes_per_row the distance between rows (at least 4 x width, often more: rows are padded),
 * width x height being the view's device size (kr_view_desc, kr_view_resize). Each pixel is 4 bytes
 * B, G, R, A, premultiplied and sRGB-encoded, the same bytes as kr_view_iosurface. Read-only.
 * The pixels stay valid and unchanged until kr_view_unlock_pixels: while they are locked,
 * kr_view_render and kr_view_resize fail with KR_ERROR_INVALID_STATE and change nothing (the
 * target kr_view_set_output2 named stays pending). kr_view_destroy unlocks them. Lock, read, unlock
 * in one host frame. The frame-complete point is kr_view_iosurface's: on return from kr_view_render
 * on karouka's own queue and on KR_BACKEND_SOFTWARE; with the host's queue, once a command buffer
 * the host committed after kr_view_render has completed.
 * KR_ERROR_INVALID_ARGUMENT: a NULL view, out_pixels or out_bytes_per_row. KR_ERROR_INVALID_STATE:
 * no frame yet (before the first render, after a resize to a new size until the next render, or
 * while renders draw into the host's textures), or the pixels are already locked.
 * KR_ERROR_INTERNAL: IOSurfaceLock failed. Nothing is written on an error. */
KR_API kr_result kr_view_lock_pixels(kr_view* view, void const** out_pixels, size_t* out_bytes_per_row);

/* Ends kr_view_lock_pixels; the pointer it returned is invalid from here.
 * KR_ERROR_INVALID_ARGUMENT: a NULL view. KR_ERROR_INVALID_STATE: the pixels are not locked. */
KR_API kr_result kr_view_unlock_pixels(kr_view* view);

/* Write the last rendered frame to a PNG file (reads the IOSurface, or the host texture it was
 * drawn into, back on the CPU). */
KR_API kr_result kr_view_capture_png(kr_view* view, char const* path);

/* Versioned by struct_size: the host sets it to sizeof(kr_view_stats) as it was compiled, and
 * kr_view_query_stats writes only the members that fit in it. Members are only ever added at the
 * end, so a host built against an older header gets the members it knows and nothing is written
 * past its struct. */
typedef struct kr_view_stats {
    uint32_t struct_size;    /* in: sizeof(kr_view_stats); left unchanged */
    uint64_t frames_rendered;
    uint64_t updates_run;    /* rendering updates run for this view's page: one per host frame that
                              * had one, whichever view's kr_view_advance_time ran it (an update
                              * covers every view of the system) */
    double last_update_ms;   /* event loop pump: tasks, timers, rAF, style, layout */
    double last_record_ms;   /* display list recording */
    double last_raster_ms;   /* Skia replay + GPU flush and submit; + the GPU wait without a host queue */
    uint64_t malloc_bytes;   /* process-wide system malloc zone bytes in use; see kr_memory_stats */
    uint64_t layout_updates; /* layout update passes run for this view's page, wherever they ran: in
                              * a rendering update (whichever view's advance ran it), a render, or
                              * script reading geometry. One per Document::update_layout that found
                              * an active document (more if an image or web face arriving made it
                              * lay out again), one per layout taken in from a flight. A pass that
                              * finds nothing to lay out still counts. */
    uint64_t event_loop_pumps; /* main-thread event loop pumps so far, process-wide: the same
                                * figure for every view, as malloc_bytes. A pump runs at most one
                                * task and the Core::Timers due; kr_view_advance_time pumps at each
                                * timer deadline it steps the host clock through and for the tasks
                                * queued, so the difference across an advance is what it cost (an
                                * idle 1 s advance of a static page: 2) */
    uint64_t last_raster_pixels; /* device pixels the last render rasterized: the area of the
                                  * viewport its frame changed (a frame replays its display list
                                  * into those rectangles, up to 8, and keeps the rest of the last
                                  * frame; a canvas, video, blinking caret or animated shader()
                                  * image counts its own bounds every frame), the whole width x height after a load, a
                                  * resize, a new target, a scroll of the document or a change it
                                  * can't bound */
    uint64_t canvas_bytes;   /* pixel bytes of every live 2D canvas's surfaces, process-wide (the
                              * same figure for every view, as malloc_bytes): 2 x width x height x 4
                              * a canvas, its draw surface and the copy last presented, plus
                              * width x height x 4 a committed transferControlToOffscreen()
                              * placeholder, its copy of the last frame. Metal
                              * textures outside the GPU cache's budget with a GPU backend (vmmap's
                              * IOAccelerator), kmalloc on KR_BACKEND_SOFTWARE. KR_MEM_TAG_CANVAS */
} kr_view_stats;

/* Fills the members of *out_stats that end within out_stats->struct_size bytes and leaves the rest
 * of the host's struct (a member a newer header added) as the host set it. Never writes more than
 * this library's sizeof(kr_view_stats). KR_ERROR_INVALID_ARGUMENT, nothing written: a NULL view or
 * out_stats, or a struct_size too small to hold frames_rendered (0: a struct the host zeroed and
 * didn't size).
 *
 * Before struct_size this was kr_view_get_stats(view, out_stats), with no struct_size
 * member. libkarouka still exports that symbol for hosts built against those headers; it writes
 * only the first six members' 48 bytes in their old layout (frames_rendered through malloc_bytes),
 * which every such header's struct holds, so layout_updates reaches only kr_view_query_stats. */
KR_API kr_result kr_view_query_stats(kr_view* view, kr_view_stats* out_stats);

/* Process-wide memory totals, by the allocator that holds them (none can be routed to the host; see
 * the host interface above). Read when called: it walks the malloc zones and the VM regions, the GC's
 * primitive storage cage entry by entry (~45 ms on a demo page). */
typedef struct kr_memory_stats {
    uint32_t struct_size;            /* in: sizeof(kr_memory_stats); left unchanged */
    uint64_t footprint_bytes;        /* the process's physical footprint, everything below included */
    uint64_t malloc_zone_bytes;      /* system malloc zones in use: Skia, CoreText, Metal, Objective-C */
    uint64_t kmalloc_bytes;          /* AK kmalloc (mimalloc: LibJS/LibWeb objects, strings, buffers),
                                      * dirty and compressed pages of its VM regions (tag 240) */
    uint64_t gc_heap_bytes;          /* the LibGC heap, likewise (VM tag 241) */
    uint64_t gpu_cache_bytes;        /* the shared Skia GPU resource cache in use */
    uint64_t gpu_cache_budget_bytes; /* its budget, kr_system_desc.gpu_cache_bytes */
    uint64_t surface_bytes;          /* every view's output IOSurface, 4 bytes per device pixel (not host textures) */
    uint64_t font_cache_strikes;     /* strikes in Skia's glyph cache (SkGraphics::GetFontCacheCountUsed), a count,
                                      * not bytes; the cache keeps at most 256 (KR_MEM_TAG_FONT_CACHE has its bytes) */
    /* Not reported separately: Skia's CPU-side caches outside malloc, Metal driver memory, the JS
     * stack and code pages. They are in footprint_bytes. */
} kr_memory_stats;

/* Fills the members of *out_stats that end within out_stats->struct_size bytes (see struct_size
 * above) and leaves the rest of the host's struct as the host set it. KR_ERROR_INVALID_ARGUMENT,
 * nothing written: a NULL system or out_stats, or a struct_size too small to hold footprint_bytes.
 * The legacy kr_system_get_memory_stats(system, out_stats), for hosts built before struct_size,
 * writes the first six members' 48 bytes in their old layout (footprint_bytes through
 * gpu_cache_budget_bytes), the size of the first header's struct, so surface_bytes and
 * font_cache_strikes reach only kr_system_query_memory_stats. */
KR_API kr_result kr_system_query_memory_stats(kr_system* system, kr_memory_stats* out_stats);

/* Bytes per subsystem, after Tessera's tsr_system_memory_usage.
 * Unlike Tessera's tags these are not a partition of the process: karouka can't route Ladybird's
 * allocations to per-tag counters without patching it, so each tag is measured where its subsystem
 * keeps its memory, and the tags overlap. KR_MEM_TAG_MALLOC_TOTAL contains the display lists, the
 * decoded images and the font cache; none of them contains the GC heap. Values may grow; new tags
 * are appended before KR_MEM_TAG_COUNT. */
typedef enum kr_mem_tag {
    /* The main thread's LibGC heap: bytes of cells in the Live state (cells that died since the last
     * collection and aren't swept yet included; the cells' external buffers are not). */
    KR_MEM_TAG_GC_HEAP_LIVE = 0,
    /* The same heap's blocks in use, 16 KiB each: live cells plus free cells in partly used blocks. */
    KR_MEM_TAG_GC_HEAP_ALLOCATED,
    /* The shared Skia GPU resource cache in use (kr_memory_stats.gpu_cache_bytes). */
    KR_MEM_TAG_GPU_CACHE,
    /* Every view's last rasterized display list: its command tape and the nested display lists it
     * draws from the view's resource storage. Not the storage's text-blob and tile caches. */
    KR_MEM_TAG_DISPLAY_LIST,
    /* Pixels of the decoded image frames alive (karouka's image decoder plugin), in kmalloc. */
    KR_MEM_TAG_DECODED_IMAGES,
    /* Skia's glyph cache as Skia counts it (SkGraphics::GetFontCacheUsed): the glyph images, paths and
     * glyph records its strikes keep. It leaves out most of what the strikes hold through CoreText and
     * CoreGraphics (each strike's scaler context, CTFont and CG bitmap context, and CG's own glyph
     * bitmap and outline caches), which is about 7-12x this on ordinary pages and more on a page that
     * animates font-size; Skia exposes nothing that would count it. Those bytes are in
     * KR_MEM_TAG_MALLOC_TOTAL. */
    KR_MEM_TAG_FONT_CACHE,
    /* System malloc zones in use plus AK kmalloc's dirty pages: kr_memory_stats.malloc_zone_bytes
     * + kmalloc_bytes. */
    KR_MEM_TAG_MALLOC_TOTAL,
    /* Every view's output IOSurface (kr_memory_stats.surface_bytes). */
    KR_MEM_TAG_SURFACES,
    /* The most KR_MEM_TAG_DECODED_IMAGES has been since the process started (a high-water mark,
     * not bytes in use now). */
    KR_MEM_TAG_DECODED_IMAGES_PEAK,
    /* Pixels of every live 2D canvas's two surfaces and every placeholder canvas's committed frame
     * (kr_view_stats.canvas_bytes): GPU memory, or
     * kmalloc on KR_BACKEND_SOFTWARE (then inside KR_MEM_TAG_MALLOC_TOTAL). */
    KR_MEM_TAG_CANVAS,
    KR_MEM_TAG_COUNT,
    KR_MEM_TAG_FORCE_I32 = 0x7fffffff
} kr_mem_tag;

/* Bytes under a tag right now, measured when called. KR_MEM_TAG_MALLOC_TOTAL queries every page of
 * kmalloc's regions (~20 ms on a demo page); the GC tags read the heap's blocks in use (~0.1 ms); the
 * others are counters. Zero for KR_MEM_TAG_COUNT and above, or a null system. Call it on the thread
 * that drives the system. */
KR_API size_t kr_system_memory_usage(kr_system const* system, kr_mem_tag tag);

/* The tag's short name ("gc_heap_live", "malloc_total", ...), or NULL for KR_MEM_TAG_COUNT and above. */
KR_API char const* kr_mem_tag_name(kr_mem_tag tag);

/*
 * Data binding: the game's state, as scalar slots the page's markup binds to.
 *
 * Each view has one model of slots addressed by model paths, Tessera's contract
 * (its data-binding spec § "2026-09-30 decision"):
 *
 *     path       = identifier ("." identifier)*          e.g. "player.health"
 *     identifier = [A-Za-z_][A-Za-z0-9_]*                 ASCII, case-sensitive
 *
 * Resolving a path interns its slot for the view's lifetime and returns an opaque handle (never 0);
 * the same path gives the same handle. The model and its handles survive loads and reloads of the
 * view, so a new document binds to the same state. Handles are view-local: resolve per view.
 *
 *     kr_binding_path health;
 *     kr_binding_path_resolve(view, "player.health", &health);
 *     kr_binding_set_number(view, health, 82.5);
 *
 * A slot is unset until its first successful setter, which fixes its type (number, bool or
 * string) for the view's lifetime; a setter of another type is KR_ERROR_INVALID_STATE. Setters run
 * no script and call nothing back. An unchanged write (numbers compare bitwise) marks nothing
 * dirty; several changed writes to one slot before the next kr_view_advance_time coalesce to the
 * last value, which that advance hands to the page.
 *
 * Fixed per-view ceilings, inclusive: KR_BINDING_MAX_PATHS paths, KR_BINDING_MAX_PATH_BYTES bytes
 * per path, KR_BINDING_MAX_STRING_BYTES bytes per stored string. Going past one is
 * KR_ERROR_LIMIT_EXCEEDED; resolving an already interned path still succeeds with a full table.
 * Every failing call changes nothing (a failed resolve leaves *out_path untouched).
 */
typedef uint32_t kr_binding_path;

#define KR_BINDING_MAX_PATHS 4096
#define KR_BINDING_MAX_PATH_BYTES 256
#define KR_BINDING_MAX_STRING_BYTES (64 * 1024)

/* KR_ERROR_INVALID_ARGUMENT for NULL arguments or a malformed path. */
KR_API kr_result kr_binding_path_resolve(kr_view* view, char const* path, kr_binding_path* out_path);
/* KR_ERROR_INVALID_ARGUMENT for a handle that is not one of this view's. */
KR_API kr_result kr_binding_set_number(kr_view* view, kr_binding_path path, double value);
KR_API kr_result kr_binding_set_bool(kr_view* view, kr_binding_path path, bool value);
/* Copies `length` bytes of UTF-8 (need not be NUL-terminated; value may be NULL when length is 0).
 * Invalid UTF-8 is KR_ERROR_INVALID_ARGUMENT. */
KR_API kr_result kr_binding_set_string(kr_view* view, kr_binding_path path, char const* value, size_t length);

/*
 * Page <-> host messages: the page's one non-standard global, `engine` (after Tessera's
 * data-binding spec § "Page ↔ game messages").
 *
 * The page sees a read-only, non-enumerable `engine`, an EventTarget with trigger(name, ...args),
 * call(name, ...args) -> Promise, on(name, listener) and off(name, listener). Arguments cross as the
 * realm's JSON.stringify of the argument array. A name must be 1..KR_ENGINE_MAX_NAME_BYTES UTF-8
 * bytes (TypeError), the JSON at most KR_ENGINE_MAX_JSON_BYTES (RangeError), and at most
 * KR_ENGINE_MAX_PENDING_CALLS calls may await an answer per document (call rejects with RangeError).
 * Without a call callback engine.call rejects; without a trigger callback engine.trigger does nothing.
 * A view has one document with a Window, so one `engine`: karouka has no iframes (an iframe,
 * frame or object element loads no document and paints as an empty box).
 *
 * The callbacks run synchronously, on the thread driving the view, from inside the page's script.
 * Strings are NUL-terminated UTF-8 borrowed until return. Inside a callback, only
 * kr_view_engine_trigger and kr_view_engine_resolve may be called.
 */
#define KR_ENGINE_MAX_NAME_BYTES 256
#define KR_ENGINE_MAX_JSON_BYTES (64 * 1024)
#define KR_ENGINE_MAX_PENDING_CALLS 256
#define KR_ENGINE_MAX_QUEUED_MESSAGES 256
#define KR_ENGINE_MAX_QUEUED_BYTES (256 * 1024)

typedef struct kr_engine_callbacks {
    uint32_t struct_size; /* sizeof(kr_engine_callbacks) */
    void (*trigger)(void* user_data, kr_view* view, char const* name, char const* json_args);
    void (*call)(void* user_data, kr_view* view, char const* name, char const* json_args, uint64_t call_id);
    void* user_data;
} kr_engine_callbacks;

/* Copies *callbacks; NULL removes both. Takes effect for the next page message.
 * KR_ERROR_INVALID_ARGUMENT, nothing changed: a NULL view, or a struct_size that can't hold trigger.
 * The legacy kr_view_set_engine_callbacks(view, callbacks), returning void, for hosts built before
 * struct_size, reads the 24 bytes of trigger, call and user_data that header had. */
KR_API kr_result kr_view_set_engine_callbacks2(kr_view* view, kr_engine_callbacks const* callbacks);

/* Host -> page event: `engine` dispatches a non-bubbling CustomEvent named `name` whose detail is the
 * parsed json_args (a JSON array; NULL or "" is []); engine.on listeners get the array spread as
 * arguments. Unparseable JSON is dropped at delivery.
 * kr_view_engine_resolve settles engine.call `call_id`: success resolves with the parsed json_value
 * (NULL or "" is undefined); failure rejects with an Error whose message is the value when it is a
 * JSON string, otherwise the JSON text. Unparseable JSON rejects with a SyntaxError.
 *
 * Both copy and enqueue on the document's `engine`. Delivery is in call order: at the document's
 * next microtask checkpoint when called from inside its callback, otherwise in a task the next
 * kr_view_advance_time runs. Loading a new document discards queued messages and pending calls.
 * KR_ERROR_INVALID_ARGUMENT: NULL view or name, a bad name length, or (resolve) a call id that is
 * unknown, already answered or from a previous document. KR_ERROR_INVALID_STATE: no document has
 * the `engine` object yet. KR_ERROR_LIMIT_EXCEEDED: json_args or json_value is longer than
 * KR_ENGINE_MAX_JSON_BYTES, or a document's queue would pass KR_ENGINE_MAX_QUEUED_MESSAGES messages
 * or KR_ENGINE_MAX_QUEUED_BYTES bytes; nothing is enqueued anywhere, and a refused resolve leaves
 * its call pending. */
KR_API kr_result kr_view_engine_trigger(kr_view* view, char const* name, char const* json_args);
KR_API kr_result kr_view_engine_resolve(kr_view* view, uint64_t call_id, bool success, char const* json_value);

#ifdef __cplusplus
}
#endif

#endif
