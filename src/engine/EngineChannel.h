/*
 * The page <-> host message channel behind the `engine` global and kr_view_engine_*
 * (include/karouka/karouka.h). Tessera's contract: Tessera's data-binding spec
 * § "Page ↔ game messages: the `engine` object"; Tessera's implementation is
 * src/core/view/engine_messages.{h,cpp}.
 *
 * One channel per view, with one endpoint: the Window of the view's document (karouka has no
 * child navigables, so a view has one Window). Each new top-level document's Window gets a fresh
 * `engine` (install_top_level()), and that discards the previous endpoint, with its queue and
 * pending calls.
 *
 * - Page -> host: engine.trigger/call serialise their argument array with the realm's
 *   JSON.stringify and reach the host callbacks synchronously, from any endpoint. The native side
 *   checks the name (1..256 UTF-8 bytes, TypeError), the JSON (<= 64 KiB, RangeError) and the
 *   endpoint's pending call count (<= 256, RangeError; engine.call turns that into a rejection).
 * - Host -> page: kr_view_engine_trigger copies (json <= 64 KiB) and enqueues on the endpoint;
 *   kr_view_engine_resolve enqueues on the endpoint that made the call. The queue holds <= 256
 *   messages, <= 256 KiB; a message it cannot take is refused. Delivery is in call order, at the next microtask checkpoint
 *   for the endpoint whose host callback is running, otherwise in a task the next
 *   kr_view_advance_time runs.
 */
#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <karouka/karouka.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Karouka {

class EngineChannel {
    AK_MAKE_NONCOPYABLE(EngineChannel);
    AK_MAKE_NONMOVABLE(EngineChannel);

public:
    static constexpr size_t max_name_bytes = 256;
    static constexpr size_t max_json_bytes = 64 * 1024;
    static constexpr size_t max_pending_calls = 256;
    static constexpr size_t max_queued_messages = 256;
    static constexpr size_t max_queued_bytes = 256 * 1024;

    explicit EngineChannel(kr_view* view)
        : m_view(view)
    {
    }
    ~EngineChannel();

    void set_callbacks(kr_engine_callbacks const* callbacks);

    // Called as each top-level Document becomes active: keeps or installs `engine` on its Window
    // and discards every other endpoint.
    void install_top_level(Web::DOM::Document&);

    kr_result host_trigger(char const* name, char const* json_args);
    kr_result host_resolve(uint64_t call_id, bool success, char const* json_value);

private:
    struct Message {
        bool is_resolve { false };
        ByteString name; // trigger: the event name
        u64 call_id { 0 };
        bool success { false };
        ByteString json;
    };

    // One Window's `engine`. Its natives and scheduled tasks hold the id, never the pointer.
    struct Endpoint {
        AK_ALLOC_WITH_KMALLOC;
        u64 id { 0 };
        GC::Root<JS::Realm> realm;
        GC::Root<JS::Object> global;
        GC::Root<JS::FunctionObject> flush;
        Vector<Message> queue;
        size_t queued_bytes { 0 };
        size_t pending_calls { 0 };
        bool task_scheduled { false };
    };

    using PageMethod = JS::ThrowCompletionOr<JS::Value> (EngineChannel::*)(JS::VM&, Endpoint&);
    JS::ThrowCompletionOr<JS::Value> page_trigger(JS::VM&, Endpoint&);
    JS::ThrowCompletionOr<JS::Value> page_call(JS::VM&, Endpoint&);
    JS::ThrowCompletionOr<JS::Value> page_take(JS::VM&, Endpoint&);
    JS::ThrowCompletionOr<void> validate(JS::VM&, ByteString const& name, ByteString const& json);

    Endpoint* endpoint(u64 id);
    Endpoint* endpoint_for(JS::Realm const&);
    Endpoint* install(Web::DOM::Document&);
    void remove_endpoints_if(Function<bool(Endpoint const&)>);
    bool can_enqueue(Endpoint const&, size_t bytes) const;
    void enqueue(Endpoint&, Message&&);
    void schedule_task(Endpoint&);

    // Cleared by the destructor; held by the scheduled tasks and the page's native functions.
    struct Alive : RefCounted<Alive> {
        bool value { true };
    };
    NonnullRefPtr<Alive> m_alive { adopt_ref(*new Alive) };

    kr_view* m_view { nullptr };
    kr_engine_callbacks m_callbacks {};

    Vector<NonnullOwnPtr<Endpoint>> m_endpoints;
    u64 m_next_endpoint_id { 1 };
    // The endpoint whose host callback is running, 0 outside callbacks.
    u64 m_calling_endpoint { 0 };

    u64 m_next_call_id { 1 };
    // engine.call ids awaiting an answer, and the endpoint that made each.
    HashMap<u64, u64> m_pending_calls;
};

}
