/*
 * _karouka.App and _karouka.AppWindow: the app shell (docs/PYTHON.md). Objective-C++, ARC.
 * (AppWindow, not Window: _karouka.Window is the IDL Window's proxy, dom.cpp.)
 *
 *     app = _karouka.App()                       # backend="metal", headless=False, verbose=False
 *     window = app.open("ui/index.html", title="Demo", size=(900, 600))
 *     app.run()                                  # returns when the last window closes, or at app.stop()
 *
 * The App owns the process's System (app.system) and its open windows (app.windows). An AppWindow is an
 * NSWindow whose content view is a CAMetalLayer pane, over a View of its size in device pixels
 * (window.view; scale: the screen's backingScaleFactor). run() runs NSApplication with the GIL
 * released: each window's display link (CADisplayLink of its view: the vsync of its screen) takes
 * the GIL, advances the engine's clock, renders, and, when the frame changed, copies the view's
 * IOSurface into the layer's next drawable. The window's title follows document.title (the title
 * open() was given while the document has none). A resize, or a move to a screen of another scale,
 * resizes the view to the layer's new device size. NSEvents go to the view through the viewer's
 * mapping (src/viewer/EventInput.h); key state goes to kr_view_set_focused.
 *
 * Headless (App(headless=True)) there is no NSApplication and no window: run() steps every open
 * window's view on a fixed 60 Hz clock, as fast as it can, which is how tests drive the App
 * (tests/python/test_app.py). run(on_frame) calls on_frame(window, now_ms) after each window's
 * frame in both modes; an exception it raises (or a KeyboardInterrupt) stops run(), which raises it.
 */

#include "module.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "../viewer/EventInput.h"

#include <algorithm>
#include <pthread.h>
#include <string.h>

using namespace Karouka::Python;

namespace {

struct AppObject;

struct WindowObject {
    PyObject_HEAD
    AppObject* app; // borrowed: the app's windows list holds the window while it is open
    PyObject* view; // _karouka.View
    PyObject* title; // str: open()'s title
    CFTypeRef controller; // KRPyWindow (CFBridgingRetain), nullptr headless or closed
    unsigned width, height; // points
    bool closed;
};

struct AppObject {
    PyObject_HEAD
    PyObject* system;
    PyObject* windows; // list of the open Windows
    PyObject* on_frame; // run()'s, else nullptr
    PyObject* error; // what a callback raised, for run() to raise
    PyThreadState* released; // while NSApplication runs between callbacks: the GIL is released
    bool headless;
    bool running;
    bool stopping;
};

}

@class KRPyWindow;

// The pane: a CAMetalLayer, flipped like the page, that hands its events to the window's view.
@interface KRPyPane : NSView
@end

@interface KRPyWindow : NSObject <NSWindowDelegate>
- (instancetype)initWithOwner:(WindowObject*)owner device:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue;
- (float)scale;
- (void)present:(bool)rendered;
- (void)showTitle:(NSString*)title;
- (void)resizeTo:(NSSize)size;
- (void)teardown;
@end

namespace {

id<MTLDevice> s_device;
id<MTLCommandQueue> s_queue;
// NSApp's delegate is weak.
id s_app_delegate;

// The View's handle, nullptr once the window or its view is closed.
kr_view* window_view(WindowObject* window)
{
    return window && !window->closed && window->view ? view_handle(window->view) : nullptr;
}

// Takes the GIL for an AppKit callback while run() has released it; a no-op when it is held
// (the callback came from inside Python, e.g. window.close()).
struct Holding {
    explicit Holding(AppObject* app)
        : m_app(app && app->released ? app : nullptr)
    {
        if (m_app) {
            PyEval_RestoreThread(m_app->released);
            m_app->released = nullptr;
        }
    }
    ~Holding()
    {
        if (m_app)
            m_app->released = PyEval_SaveThread();
    }
    Holding(Holding const&) = delete;
    Holding& operator=(Holding const&) = delete;

private:
    AppObject* m_app;
};

void app_stop(AppObject* app)
{
    if (!app->running || app->stopping)
        return;
    app->stopping = true;
    if (app->headless)
        return;
    [NSApp stop:nil];
    // stop: takes effect after the next event.
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                       timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0]
             atStart:YES];
}

// Keeps the first exception a callback raised for run(), and stops.
void app_fail(AppObject* app)
{
    if (!app->error)
        app->error = PyErr_GetRaisedException();
    else
        PyErr_Clear();
    app_stop(app);
}

// Closes the window (idempotent): its NSWindow, its view, and its place in the app.
void window_shutdown(WindowObject* window)
{
    if (window->closed)
        return;
    window->closed = true;
    Py_INCREF(window);
    PyObject* raised = PyErr_GetRaisedException();
    if (window->controller) {
        KRPyWindow* controller = CFBridgingRelease(window->controller);
        window->controller = nullptr;
        [controller teardown];
    }
    if (PyObject* result = PyObject_CallMethod(window->view, "close", nullptr))
        Py_DECREF(result);
    else
        PyErr_WriteUnraisable(window->view);
    if (AppObject* app = window->app) {
        window->app = nullptr;
        Py_ssize_t index = PySequence_Index(app->windows, reinterpret_cast<PyObject*>(window));
        if (index >= 0)
            PySequence_DelItem(app->windows, index);
        else
            PyErr_Clear();
        if (PyList_GET_SIZE(app->windows) == 0)
            app_stop(app);
    }
    PyErr_SetRaisedException(raised);
    Py_DECREF(window);
}

// One frame of one window: the engine's clock, a render, the drawable, the title, then on_frame.
void window_frame(WindowObject* window, double now_ms)
{
    AppObject* app = window->app;
    kr_view* view = window_view(window);
    if (!app || !view)
        return;
    Py_INCREF(window);
    if (on_main_thread(Py_TYPE(window))) {
        kr_view_advance_time(view, now_ms);
        // A page script may have closed the window or its view.
        if ((view = window_view(window))) {
            bool rendered = false;
            kr_view_render(view, &rendered);
            if (window->controller) {
                auto* controller = (__bridge KRPyWindow*)window->controller;
                [controller present:rendered];
                char const* title = kr_view_title(view);
                [controller showTitle:title && *title ? @(title) : @(PyUnicode_AsUTF8(window->title))];
            }
        }
        if (app->on_frame && !app->error) {
            PyObject* result = PyObject_CallFunction(app->on_frame, "Od", reinterpret_cast<PyObject*>(window), now_ms);
            if (result)
                Py_DECREF(result);
            else
                app_fail(app);
        }
    } else {
        app_fail(app);
    }
    if (PyErr_CheckSignals() < 0)
        app_fail(app);
    Py_DECREF(window);
}

}

@interface KRPyAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) AppObject* app;
@end

@implementation KRPyAppDelegate

// Cmd+Q closes every window, which returns from run(); the process stays the Python app's.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    if (AppObject* app = _app; app && app->running) {
        Holding holding(app);
        PyObject* windows = PySequence_List(app->windows);
        if (windows) {
            for (Py_ssize_t i = 0; i < PyList_GET_SIZE(windows); ++i)
                window_shutdown(reinterpret_cast<WindowObject*>(PyList_GET_ITEM(windows, i)));
            Py_DECREF(windows);
        } else {
            PyErr_Clear();
        }
        app_stop(app);
    }
    return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return NO; }

@end

@implementation KRPyPane {
@public
    WindowObject* _owner;
    float _scale;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }

- (void)updateTrackingAreas
{
    for (NSTrackingArea* area in self.trackingAreas)
        [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
                                                       options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
    [super updateTrackingAreas];
}

// Input is queued for the next frame (kr_view_inject_input2 runs no script), so no GIL is needed.
- (void)send:(kr_input_event const&)input
{
    if (kr_view* view = window_view(_owner))
        kr_view_inject_input2(view, &input);
}

- (void)sendMouse:(NSEvent*)event type:(kr_input_type)type button:(uint32_t)button
{
    [self send:Karouka::EventInput::mouse(self, event, type, button, _scale)];
}

- (void)mouseMoved:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_MOVE button:0]; }
- (void)mouseDragged:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_MOVE button:0]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_MOVE button:1]; }
- (void)mouseDown:(NSEvent*)event
{
    [self.window makeFirstResponder:self];
    [self sendMouse:event type:KR_INPUT_MOUSE_DOWN button:0];
}
- (void)mouseUp:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_UP button:0]; }
- (void)rightMouseDown:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_DOWN button:1]; }
- (void)rightMouseUp:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_UP button:1]; }
- (void)otherMouseDown:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_DOWN button:2]; }
- (void)otherMouseUp:(NSEvent*)event { [self sendMouse:event type:KR_INPUT_MOUSE_UP button:2]; }
- (void)scrollWheel:(NSEvent*)event { [self send:Karouka::EventInput::wheel(self, event, _scale)]; }
- (void)keyDown:(NSEvent*)event { [self send:Karouka::EventInput::key(event, KR_INPUT_KEY_DOWN)]; }
- (void)keyUp:(NSEvent*)event { [self send:Karouka::EventInput::key(event, KR_INPUT_KEY_UP)]; }

@end

@implementation KRPyWindow {
    WindowObject* _owner;
    NSWindow* _window;
    KRPyPane* _pane;
    CADisplayLink* _link;
    id<MTLCommandQueue> _queue;
    id<MTLTexture> _source;
    IOSurfaceRef _source_surface;
    bool _needs_present;
    bool _closing; // AppKit is closing the NSWindow already
}

- (instancetype)initWithOwner:(WindowObject*)owner device:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue
{
    if (!(self = [super init]))
        return nil;
    _owner = owner;
    _queue = queue;
    NSRect frame = NSMakeRect(0, 0, owner->width, owner->height);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.releasedWhenClosed = NO;
    _window.title = @(PyUnicode_AsUTF8(owner->title));
    _pane = [[KRPyPane alloc] initWithFrame:frame];
    _pane.wantsLayer = YES;
    _pane->_owner = owner;
    _pane->_scale = (float)_window.backingScaleFactor;
    auto* layer = (CAMetalLayer*)_pane.layer;
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO; // a blit destination
    layer.contentsGravity = kCAGravityTopLeft;
    layer.contentsScale = _pane->_scale;
    layer.drawableSize = CGSizeMake(frame.size.width * _pane->_scale, frame.size.height * _pane->_scale);
    _window.contentView = _pane;
    _window.delegate = self;
    [_window makeFirstResponder:_pane];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    // Frames follow the vsync of the window's screen; common modes keep them through live resizes
    // and menus. The link retains its target until -teardown invalidates it.
    _link = [_pane displayLinkWithTarget:self selector:@selector(step:)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 60);
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    _needs_present = true;
    return self;
}

- (float)scale
{
    return _pane->_scale;
}

- (void)step:(CADisplayLink*)link
{
    AppObject* app = _owner ? _owner->app : nullptr;
    // Only inside run(): anything else spinning the main run loop doesn't hold this GIL.
    if (!app || !app->running || app->stopping)
        return;
    @autoreleasepool {
        Holding holding(app);
        window_frame(_owner, CACurrentMediaTime() * 1000.0);
    }
}

// The view's IOSurface (complete: without the host's queue kr_view_render waits for the GPU, and
// the software backend draws on the CPU) into the next drawable, when it changed or the drawable
// size did.
- (void)present:(bool)rendered
{
    if (!rendered && !_needs_present)
        return;
    kr_view* view = window_view(_owner);
    auto surface = view ? (IOSurfaceRef)kr_view_iosurface(view) : nullptr;
    if (!surface)
        return;
    if (surface != _source_surface) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:IOSurfaceGetWidth(surface)
                                                                                       height:IOSurfaceGetHeight(surface)
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        _source = [_queue.device newTextureWithDescriptor:desc iosurface:surface plane:0];
        _source_surface = surface;
    }
    id<CAMetalDrawable> drawable = [(CAMetalLayer*)_pane.layer nextDrawable];
    if (!_source || !drawable) {
        _needs_present = true;
        return;
    }
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    id<MTLTexture> target = drawable.texture; // not to be read once the drawable is presented
    NSUInteger width = std::min(_source.width, target.width);
    NSUInteger height = std::min(_source.height, target.height);
    // A surface smaller than the drawable (a resize the view hasn't rendered yet) presents again.
    _needs_present = width != target.width || height != target.height;
    [blit copyFromTexture:_source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:target destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
    // The next render rewrites the surface in place: the copy finishes first.
    [commands waitUntilCompleted];
}

- (void)showTitle:(NSString*)title
{
    if (![_window.title isEqualToString:title])
        _window.title = title;
}

- (void)resizeTo:(NSSize)size
{
    [_window setContentSize:size];
    [self layout];
}

// The pane's size or the screen's scale changed: the layer and the view follow, in device pixels.
- (void)layout
{
    NSSize size = _pane.bounds.size;
    float scale = (float)_window.backingScaleFactor;
    _pane->_scale = scale;
    auto* layer = (CAMetalLayer*)_pane.layer;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
    _owner->width = (unsigned)size.width;
    _owner->height = (unsigned)size.height;
    if (kr_view* view = window_view(_owner))
        kr_view_resize(view, (uint32_t)(size.width * scale), (uint32_t)(size.height * scale), scale);
    _needs_present = true;
}

- (void)windowDidResize:(NSNotification*)notification { [self layout]; }
- (void)windowDidChangeBackingProperties:(NSNotification*)notification { [self layout]; }

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    if (kr_view* view = window_view(_owner))
        kr_view_set_focused(view, true);
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    if (kr_view* view = window_view(_owner))
        kr_view_set_focused(view, false);
}

// The close button or Cmd+W (window.close() tears down first, so it doesn't come here).
- (void)windowWillClose:(NSNotification*)notification
{
    WindowObject* owner = _owner;
    if (!owner)
        return;
    __attribute__((objc_precise_lifetime)) KRPyWindow* keep = self;
    (void)keep;
    _closing = true;
    // AppKit closes windows only while run() runs NSApplication, with the GIL released.
    if (owner->app && owner->app->released) {
        Holding holding(owner->app);
        window_shutdown(owner);
    }
}

- (void)teardown
{
    [_link invalidate];
    _link = nil;
    _window.delegate = nil;
    _pane->_owner = nullptr;
    _owner = nullptr;
    if (!_closing) {
        [_window orderOut:nil];
        [_window close];
    }
}

@end

namespace {

// ---- AppWindow ----

PyObject* window_close(PyObject* object, PyObject*)
{
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    @autoreleasepool {
        window_shutdown(reinterpret_cast<WindowObject*>(object));
    }
    if (PyErr_Occurred())
        return nullptr;
    Py_RETURN_NONE;
}

PyObject* window_resize(PyObject* object, PyObject* args)
{
    auto* self = reinterpret_cast<WindowObject*>(object);
    unsigned width = 0;
    unsigned height = 0;
    if (!PyArg_ParseTuple(args, "II:resize", &width, &height))
        return nullptr;
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    kr_view* view = window_view(self);
    if (!view)
        return raise_error(Py_TYPE(object), "the window is closed", static_cast<int>(KR_ERROR_INVALID_STATE));
    if (!width || !height) {
        PyErr_SetString(PyExc_ValueError, "resize() needs a nonzero width and height");
        return nullptr;
    }
    if (self->controller) {
        @autoreleasepool {
            [(__bridge KRPyWindow*)self->controller resizeTo:NSMakeSize(width, height)];
        }
    } else {
        self->width = width;
        self->height = height;
        kr_view_resize(view, width, height, 1);
    }
    Py_RETURN_NONE;
}

PyObject* window_get_view(PyObject* object, void*)
{
    return Py_NewRef(reinterpret_cast<WindowObject*>(object)->view);
}

PyObject* window_get_title(PyObject* object, void*)
{
    auto* self = reinterpret_cast<WindowObject*>(object);
    if (!self->closed) {
        if (!on_main_thread(Py_TYPE(object)))
            return nullptr;
        if (kr_view* view = window_view(self)) {
            char const* title = kr_view_title(view);
            if (title && *title)
                return PyUnicode_DecodeUTF8(title, static_cast<Py_ssize_t>(strlen(title)), "replace");
        }
    }
    return Py_NewRef(self->title);
}

PyObject* window_get_closed(PyObject* object, void*)
{
    return PyBool_FromLong(reinterpret_cast<WindowObject*>(object)->closed);
}

PyObject* window_get_size(PyObject* object, void*)
{
    auto* self = reinterpret_cast<WindowObject*>(object);
    return Py_BuildValue("(II)", self->width, self->height);
}

PyObject* window_get_scale(PyObject* object, void*)
{
    auto* self = reinterpret_cast<WindowObject*>(object);
    double scale = 1;
    if (self->controller)
        scale = [(__bridge KRPyWindow*)self->controller scale];
    return PyFloat_FromDouble(scale);
}

void window_dealloc(PyObject* object)
{
    auto* self = reinterpret_cast<WindowObject*>(object);
    PyTypeObject* type = Py_TYPE(object);
    // Only a window whose app went first can still be open here.
    if (self->controller) {
        KRPyWindow* controller = CFBridgingRelease(self->controller);
        self->controller = nullptr;
        if (pthread_main_np())
            [controller teardown];
        else
            dispatch_async(dispatch_get_main_queue(), ^{ [controller teardown]; });
    }
    Py_CLEAR(self->view); // the View queues itself off the main thread
    Py_CLEAR(self->title);
    type->tp_free(object);
    Py_DECREF(type);
}

PyMethodDef window_methods[] = {
    { "close", window_close, METH_NOARGS,
        PyDoc_STR("close()\n\nClose the window and its view; the app's run() returns when its last window closes.") },
    { "resize", window_resize, METH_VARARGS,
        PyDoc_STR("resize(width, height)\n\nResize the window's content to width x height points (the view follows).") },
    { nullptr, nullptr, 0, nullptr },
};

PyGetSetDef window_getset[] = {
    { "view", window_get_view, nullptr, PyDoc_STR("The window's View (closed with the window)."), nullptr },
    { "title", window_get_title, nullptr, PyDoc_STR("document.title, or open()'s title while the document has none."), nullptr },
    { "closed", window_get_closed, nullptr, PyDoc_STR("Whether the window is closed."), nullptr },
    { "size", window_get_size, nullptr, PyDoc_STR("(width, height) of the content, in points."), nullptr },
    { "scale", window_get_scale, nullptr, PyDoc_STR("Device pixels per point (the screen's; 1.0 headless)."), nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr },
};

PyType_Slot window_slots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(window_dealloc) },
    { Py_tp_methods, window_methods },
    { Py_tp_getset, window_getset },
    { Py_tp_doc, const_cast<char*>(PyDoc_STR("A native window of an App (App.open): one page, its View, in one NSWindow.")) },
    { 0, nullptr },
};

PyType_Spec window_spec = {
    "_karouka.AppWindow",
    sizeof(WindowObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    window_slots,
};

// ---- App ----

PyObject* app_new(PyTypeObject* type, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "backend", "headless", "verbose", nullptr };
    char const* backend = "metal";
    int headless = 0;
    int verbose = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$spp:App", keywords, &backend, &headless, &verbose))
        return nullptr;
    if (!on_main_thread(type))
        return nullptr;
    ModuleState* state = state_of_type(type);
    if (!state)
        return nullptr;
    if (!headless) {
        @autoreleasepool {
            if (!s_device) {
                s_device = MTLCreateSystemDefaultDevice();
                s_queue = [s_device newCommandQueue];
            }
            if (!s_queue)
                return raise_error(type, "App: no Metal device for the windows", static_cast<int>(KR_ERROR_GPU));
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }
    }
    PyObject* system_kwargs = Py_BuildValue("{sssO}", "backend", backend, "verbose", verbose ? Py_True : Py_False);
    if (!system_kwargs)
        return nullptr;
    PyObject* empty = PyTuple_New(0);
    PyObject* system = empty ? PyObject_Call(reinterpret_cast<PyObject*>(state->system_type), empty, system_kwargs) : nullptr;
    Py_XDECREF(empty);
    Py_DECREF(system_kwargs);
    if (!system)
        return nullptr;
    auto* self = reinterpret_cast<AppObject*>(type->tp_alloc(type, 0));
    if (!self) {
        Py_DECREF(system);
        return nullptr;
    }
    self->system = system;
    self->windows = PyList_New(0);
    self->headless = headless;
    if (!self->windows) {
        Py_DECREF(self);
        return nullptr;
    }
    return reinterpret_cast<PyObject*>(self);
}

PyObject* app_open(PyObject* object, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "path", "title", "size", nullptr };
    auto* self = reinterpret_cast<AppObject*>(object);
    PyTypeObject* type = Py_TYPE(object);
    PyObject* path = nullptr;
    PyObject* title = Py_None;
    unsigned width = 1024;
    unsigned height = 768;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O(II):open", keywords, &path, &title, &width, &height))
        return nullptr;
    if (title != Py_None && !PyUnicode_Check(title)) {
        PyErr_SetString(PyExc_TypeError, "open(): title must be a str or None");
        return nullptr;
    }
    if (!width || !height) {
        PyErr_SetString(PyExc_ValueError, "open(): size needs a nonzero width and height");
        return nullptr;
    }
    if (!on_main_thread(type))
        return nullptr;
    ModuleState* state = state_of_type(type);
    if (!state)
        return nullptr;
    @autoreleasepool {
        double scale = self->headless ? 1.0 : NSScreen.mainScreen.backingScaleFactor;
        PyObject* view = PyObject_CallMethod(self->system, "view", "IIf", static_cast<unsigned>(width * scale),
            static_cast<unsigned>(height * scale), static_cast<float>(scale));
        if (!view)
            return nullptr;
        PyObject* loaded = PyObject_CallMethod(view, "load_file", "O", path);
        if (!loaded) {
            PyObject* raised = PyErr_GetRaisedException();
            Py_XDECREF(PyObject_CallMethod(view, "close", nullptr));
            PyErr_Clear();
            Py_DECREF(view);
            PyErr_SetRaisedException(raised);
            return nullptr;
        }
        Py_DECREF(loaded);
        // The title until the document has one: open()'s, else the file's name.
        PyObject* shown = nullptr;
        if (title != Py_None) {
            shown = Py_NewRef(title);
        } else if (PyObject* fspath = PyOS_FSPath(path)) {
            PyObject* name = PyObject_Str(fspath);
            Py_DECREF(fspath);
            if (name) {
                Py_ssize_t slash = PyUnicode_FindChar(name, '/', 0, PyUnicode_GET_LENGTH(name), -1);
                shown = slash >= 0 ? PyUnicode_Substring(name, slash + 1, PyUnicode_GET_LENGTH(name)) : Py_NewRef(name);
                Py_DECREF(name);
            }
        }
        auto* window = shown ? reinterpret_cast<WindowObject*>(state->window_type->tp_alloc(state->window_type, 0)) : nullptr;
        if (!window) {
            Py_XDECREF(shown);
            Py_XDECREF(PyObject_CallMethod(view, "close", nullptr));
            Py_DECREF(view);
            return nullptr;
        }
        window->app = self;
        window->view = view;
        window->title = shown;
        window->width = width;
        window->height = height;
        if (PyList_Append(self->windows, reinterpret_cast<PyObject*>(window)) < 0) {
            window->app = nullptr;
            window_shutdown(window);
            Py_DECREF(window);
            return nullptr;
        }
        if (!self->headless) {
            KRPyWindow* controller = [[KRPyWindow alloc] initWithOwner:window device:s_device queue:s_queue];
            window->controller = CFBridgingRetain(controller);
            // The screen it opened on may not be the main one.
            if ([controller scale] != scale)
                [controller resizeTo:NSMakeSize(width, height)];
        }
        return reinterpret_cast<PyObject*>(window);
    }
}

PyObject* app_run(PyObject* object, PyObject* args, PyObject* kwargs)
{
    static char const* const keywords[] = { "on_frame", nullptr };
    auto* self = reinterpret_cast<AppObject*>(object);
    PyObject* on_frame = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O:run", keywords, &on_frame))
        return nullptr;
    if (on_frame != Py_None && !PyCallable_Check(on_frame)) {
        PyErr_SetString(PyExc_TypeError, "run(): on_frame must be callable or None");
        return nullptr;
    }
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    if (self->running)
        return raise_error(Py_TYPE(object), "run(): the app is already running", static_cast<int>(KR_ERROR_INVALID_STATE));
    if (PyList_GET_SIZE(self->windows) == 0)
        Py_RETURN_NONE;
    Py_INCREF(object);
    self->on_frame = on_frame == Py_None ? nullptr : Py_NewRef(on_frame);
    self->running = true;
    self->stopping = false;
    if (self->headless) {
        // 60 frames a second of engine time, back to back.
        for (uint64_t frame = 1; !self->stopping && PyList_GET_SIZE(self->windows); ++frame) {
            PyObject* windows = PySequence_List(self->windows);
            if (!windows) {
                app_fail(self);
                break;
            }
            for (Py_ssize_t i = 0; i < PyList_GET_SIZE(windows) && !self->stopping; ++i)
                window_frame(reinterpret_cast<WindowObject*>(PyList_GET_ITEM(windows, i)), frame * 1000.0 / 60.0);
            Py_DECREF(windows);
        }
    } else {
        @autoreleasepool {
            KRPyAppDelegate* delegate = [KRPyAppDelegate new];
            delegate.app = self;
            s_app_delegate = delegate;
            NSApp.delegate = delegate;
            if (!NSApp.mainMenu) {
                NSMenu* bar = [NSMenu new];
                NSMenuItem* item = [NSMenuItem new];
                NSMenu* menu = [NSMenu new];
                [menu addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
                [menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
                item.submenu = menu;
                [bar addItem:item];
                NSApp.mainMenu = bar;
            }
            [NSApp activateIgnoringOtherApps:YES];
        }
        self->released = PyEval_SaveThread();
        [NSApp run];
        PyEval_RestoreThread(self->released);
        self->released = nullptr;
        ((KRPyAppDelegate*)s_app_delegate).app = nullptr;
    }
    self->running = false;
    self->stopping = false;
    Py_CLEAR(self->on_frame);
    PyObject* error = self->error;
    self->error = nullptr;
    Py_DECREF(object);
    if (error) {
        PyErr_SetRaisedException(error);
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* app_stop_method(PyObject* object, PyObject*)
{
    if (!on_main_thread(Py_TYPE(object)))
        return nullptr;
    app_stop(reinterpret_cast<AppObject*>(object));
    Py_RETURN_NONE;
}

PyObject* app_get_system(PyObject* object, void*)
{
    return Py_NewRef(reinterpret_cast<AppObject*>(object)->system);
}

PyObject* app_get_windows(PyObject* object, void*)
{
    return PySequence_List(reinterpret_cast<AppObject*>(object)->windows);
}

PyObject* app_get_headless(PyObject* object, void*)
{
    return PyBool_FromLong(reinterpret_cast<AppObject*>(object)->headless);
}

void app_dealloc(PyObject* object)
{
    auto* self = reinterpret_cast<AppObject*>(object);
    PyTypeObject* type = Py_TYPE(object);
    // Open windows close with their app (on the main thread; elsewhere they close as they go).
    if (self->windows) {
        PyObject* raised = PyErr_GetRaisedException();
        while (PyList_GET_SIZE(self->windows)) {
            auto* window = reinterpret_cast<WindowObject*>(PyList_GET_ITEM(self->windows, 0));
            if (pthread_main_np()) {
                window_shutdown(window);
                if (PyList_GET_SIZE(self->windows) && PyList_GET_ITEM(self->windows, 0) == reinterpret_cast<PyObject*>(window))
                    PySequence_DelItem(self->windows, 0);
            } else {
                window->app = nullptr;
                PySequence_DelItem(self->windows, 0);
            }
        }
        PyErr_SetRaisedException(raised);
    }
    Py_CLEAR(self->windows);
    Py_CLEAR(self->on_frame);
    Py_CLEAR(self->error);
    Py_CLEAR(self->system);
    type->tp_free(object);
    Py_DECREF(type);
}

PyMethodDef app_methods[] = {
    { "open", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(app_open)), METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("open(path, title=None, size=(1024, 768)) -> AppWindow\n\n"
                  "Load a page from disk into a new window of size points (title: the window's until the document\n"
                  "has one; None: the file's name).") },
    { "run", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(app_run)), METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("run(on_frame=None)\n\n"
                  "Run the windows until the last one closes or stop(): NSApplication with a display-link frame\n"
                  "per window (headless: a 60 Hz clock, back to back). on_frame(window, now_ms) is called after\n"
                  "each window's frame; what it raises stops run() and is raised here.") },
    { "stop", app_stop_method, METH_NOARGS,
        PyDoc_STR("stop()\n\nReturn from run() after the current frame; the windows stay open.") },
    { nullptr, nullptr, 0, nullptr },
};

PyGetSetDef app_getset[] = {
    { "system", app_get_system, nullptr, PyDoc_STR("The app's System (one per process)."), nullptr },
    { "windows", app_get_windows, nullptr, PyDoc_STR("The open windows, in the order they opened."), nullptr },
    { "headless", app_get_headless, nullptr, PyDoc_STR("Whether the app has no NSApplication and no windows on screen."), nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr },
};

PyType_Slot app_slots[] = {
    { Py_tp_new, reinterpret_cast<void*>(app_new) },
    { Py_tp_dealloc, reinterpret_cast<void*>(app_dealloc) },
    { Py_tp_methods, app_methods },
    { Py_tp_getset, app_getset },
    { Py_tp_doc, const_cast<char*>(PyDoc_STR(
                     "App(*, backend='metal', headless=False, verbose=False)\n\n"
                     "The app shell: the process's System and its native windows (App.open, App.run)."))
    },
    { 0, nullptr },
};

PyType_Spec app_spec = {
    "_karouka.App",
    sizeof(AppObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE,
    app_slots,
};

}

int Karouka::Python::app_exec(PyObject* module, ModuleState& state)
{
    state.window_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &window_spec, nullptr));
    if (!state.window_type || PyModule_AddType(module, state.window_type) < 0)
        return -1;
    state.app_type = reinterpret_cast<PyTypeObject*>(PyType_FromModuleAndSpec(module, &app_spec, nullptr));
    if (!state.app_type || PyModule_AddType(module, state.app_type) < 0)
        return -1;
    return 0;
}

int Karouka::Python::app_traverse(ModuleState& state, visitproc visit, void* arg)
{
    Py_VISIT(state.app_type);
    Py_VISIT(state.window_type);
    return 0;
}

void Karouka::Python::app_clear(ModuleState& state)
{
    Py_CLEAR(state.app_type);
    Py_CLEAR(state.window_type);
}
