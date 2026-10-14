/*
 * The `engine` page <-> host message channel; see EngineChannel.h.
 */

#include "EngineChannel.h"
#include "HostInterface.h"

#include <AK/Utf16String.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Script.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>

namespace Karouka {

// The page side, run once per Window as a classic script. It evaluates to a function that takes the
// Window and the native functions and returns `flush`, which drains the host queue into events and settled calls.
static constexpr auto bootstrap_source = R"JS((function (global, hostTrigger, hostCall, hostTake, hostQueued) {
  "use strict";
  const target = new EventTarget();
  const stringify = JSON.stringify, parse = JSON.parse, isArray = Array.isArray;
  const CE = CustomEvent, P = Promise, M = Map, Err = Error, TypeErr = TypeError;
  const pending = new M();
  const wrappers = new M();
  let flushQueued = false;
  function flush() {
    flushQueued = false;
    const batch = hostTake();
    for (let i = 0; i < batch.length; ++i) {
      const m = batch[i];
      if (m[0] === 0) {
        let detail;
        try { detail = m[2].length ? parse(m[2]) : []; } catch (e) { continue; }
        if (!isArray(detail)) detail = [detail];
        target.dispatchEvent(new CE(m[1], { detail }));
      } else {
        const p = pending.get(m[1]);
        if (!p) continue;
        pending.delete(m[1]);
        let value;
        try { value = m[3].length ? parse(m[3]) : undefined; } catch (e) { p.reject(e); continue; }
        if (m[2]) p.resolve(value);
        else p.reject(new Err(typeof value === "string" ? value : m[3]));
      }
    }
  }
  function kick() {
    if (!flushQueued && hostQueued() > 0) { flushQueued = true; P.resolve().then(flush); }
  }
  function def(name, fn) {
    Object.defineProperty(target, name, { value: fn, writable: true, enumerable: false, configurable: true });
  }
  def("trigger", function trigger(name, ...args) {
    hostTrigger(name, stringify(args));
    kick();
  });
  def("call", function call(name, ...args) {
    return new P((resolve, reject) => {
      const id = hostCall(name, stringify(args));
      pending.set(id, { resolve, reject });
      kick();
    });
  });
  def("on", function on(name, listener) {
    if (typeof listener !== "function") throw new TypeErr("engine.on: listener is not a function");
    name = String(name);
    let byName = wrappers.get(name);
    if (!byName) wrappers.set(name, byName = new M());
    if (byName.has(listener)) return;
    const wrapper = (event) => { listener.apply(target, event.detail); };
    byName.set(listener, wrapper);
    target.addEventListener(name, wrapper);
  });
  def("off", function off(name, listener) {
    name = String(name);
    const byName = wrappers.get(name);
    const wrapper = byName && byName.get(listener);
    if (!wrapper) return;
    byName.delete(listener);
    target.removeEventListener(name, wrapper);
  });
  Object.defineProperty(global, "engine", { value: target, writable: false, enumerable: false, configurable: false });
  return flush;
}))JS"sv;

EngineChannel::~EngineChannel()
{
    // Invalidates the scheduled tasks and the native functions still reachable from the pages.
    m_alive->value = false;
}

void EngineChannel::set_callbacks(kr_engine_callbacks const* callbacks)
{
    m_callbacks = callbacks ? *callbacks : kr_engine_callbacks {};
}

EngineChannel::Endpoint* EngineChannel::endpoint(u64 id)
{
    for (auto& endpoint : m_endpoints) {
        if (endpoint->id == id)
            return endpoint.ptr();
    }
    return nullptr;
}

EngineChannel::Endpoint* EngineChannel::endpoint_for(JS::Realm const& realm)
{
    for (auto& endpoint : m_endpoints) {
        if (endpoint->realm.ptr() == &realm)
            return endpoint.ptr();
    }
    return nullptr;
}

void EngineChannel::remove_endpoints_if(Function<bool(Endpoint const&)> predicate)
{
    m_endpoints.remove_all_matching([&](auto& endpoint) {
        if (!predicate(*endpoint))
            return false;
        auto id = endpoint->id;
        m_pending_calls.remove_all_matching([id](auto, auto owner) { return owner == id; });
        return true;
    });
}

void EngineChannel::install_top_level(Web::DOM::Document& document)
{
    auto& realm = Web::HTML::relevant_realm(document);
    // The Window may have its `engine` already: DOMParser / createHTMLDocument documents share
    // their creator's Window.
    if (endpoint_for(realm))
        return;
    remove_endpoints_if([](Endpoint const&) { return true; });
    install(document);
}

EngineChannel::Endpoint* EngineChannel::install(Web::DOM::Document& document)
{
    // A top-level document's global is always its Window. (No is<Window> check: Window's type info is not exported from LibWeb, so the dynamic_cast behind
    // it fails here.)
    auto& realm = Web::HTML::relevant_realm(document);
    auto& vm = realm.vm();
    auto id = m_next_endpoint_id++;
    auto alive = m_alive;

    Web::HTML::TemporaryExecutionContext context(realm);
    auto native = [&](Utf16FlyString name, i32 length, PageMethod method) {
        return JS::NativeFunction::create(realm, [this, alive, id, method](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto* endpoint = alive->value ? this->endpoint(id) : nullptr;
            if (!endpoint)
                return vm.throw_completion<JS::Error>("engine: this document's channel is closed"sv);
            return (this->*method)(vm, *endpoint);
        },
            length, name);
    };
    auto trigger = native("trigger"_utf16_fly_string, 2, &EngineChannel::page_trigger);
    auto call = native("call"_utf16_fly_string, 2, &EngineChannel::page_call);
    auto take = native("take"_utf16_fly_string, 0, &EngineChannel::page_take);
    auto queued = JS::NativeFunction::create(realm, [this, alive, id](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        auto* endpoint = alive->value ? this->endpoint(id) : nullptr;
        return JS::Value(endpoint ? static_cast<double>(endpoint->queue.size()) : 0.0);
    },
        0, "queued"_utf16_fly_string);

    // Not a ClassicScript: the new document is not yet its Window's associated Document, so "can run
    // script" would refuse. The bootstrap only defines properties; it runs no page code. It defines
    // `engine` on the Window itself: globalThis is the WindowProxy, whose same-origin check needs
    // the associated Document a just-created one does not have yet.
    auto script = JS::Script::parse(Utf16String::from_utf8(bootstrap_source), realm, "karouka:engine"sv);
    if (script.is_error()) {
        Karouka::log(KR_LOG_ERROR, "could not parse the engine bootstrap");
        return nullptr;
    }
    auto completion = vm.run(*script.value());
    if (completion.is_error() || !completion.value().is_function()) {
        Karouka::log(KR_LOG_ERROR, "could not install the engine object");
        return nullptr;
    }
    auto flush = JS::call(vm, completion.value(), JS::js_undefined(), &realm.global_object(), trigger, call, take, queued);
    if (flush.is_error() || !flush.value().is_function()) {
        Karouka::log(KR_LOG_ERROR, "could not install the engine object");
        return nullptr;
    }
    auto endpoint = make<Endpoint>();
    endpoint->id = id;
    endpoint->realm = GC::make_root(realm);
    endpoint->global = GC::make_root(realm.global_object());
    auto flush_value = flush.release_value();
    endpoint->flush = GC::make_root(flush_value.as_function());
    m_endpoints.append(move(endpoint));
    return m_endpoints.last().ptr();
}

JS::ThrowCompletionOr<void> EngineChannel::validate(JS::VM& vm, ByteString const& name, ByteString const& json)
{
    if (name.is_empty() || name.length() > max_name_bytes || name.contains('\0'))
        return vm.throw_completion<JS::TypeError>("engine: the message name must be 1 to 256 UTF-8 bytes"sv);
    if (json.length() > max_json_bytes)
        return vm.throw_completion<JS::RangeError>("engine: the serialised arguments exceed 64 KiB"sv);
    return {};
}

static JS::ThrowCompletionOr<ByteString> utf8_argument(JS::VM& vm, size_t index)
{
    auto string = TRY(vm.argument(index).to_utf16_string(vm));
    return string.to_utf8().to_byte_string();
}

JS::ThrowCompletionOr<JS::Value> EngineChannel::page_trigger(JS::VM& vm, Endpoint& endpoint)
{
    auto name = TRY(utf8_argument(vm, 0));
    auto json = TRY(utf8_argument(vm, 1));
    TRY(validate(vm, name, json));
    if (!m_callbacks.trigger)
        return JS::js_undefined();
    auto outer = exchange(m_calling_endpoint, endpoint.id);
    m_callbacks.trigger(m_callbacks.user_data, m_view, name.characters(), json.characters());
    m_calling_endpoint = outer;
    return JS::js_undefined();
}

JS::ThrowCompletionOr<JS::Value> EngineChannel::page_call(JS::VM& vm, Endpoint& endpoint)
{
    auto name = TRY(utf8_argument(vm, 0));
    auto json = TRY(utf8_argument(vm, 1));
    TRY(validate(vm, name, json));
    if (!m_callbacks.call)
        return vm.throw_completion<JS::Error>("engine.call: the host handles no calls"sv);
    if (endpoint.pending_calls >= max_pending_calls)
        return vm.throw_completion<JS::RangeError>("engine.call: more than 256 pending calls"sv);
    auto id = m_next_call_id++;
    m_pending_calls.set(id, endpoint.id);
    ++endpoint.pending_calls;
    auto endpoint_id = endpoint.id;
    auto outer = exchange(m_calling_endpoint, endpoint_id);
    // The callback may answer (and so delete nothing here: enqueue never removes endpoints).
    m_callbacks.call(m_callbacks.user_data, m_view, name.characters(), json.characters(), id);
    m_calling_endpoint = outer;
    return JS::Value(static_cast<double>(id));
}

JS::ThrowCompletionOr<JS::Value> EngineChannel::page_take(JS::VM& vm, Endpoint& endpoint)
{
    auto& realm = *vm.current_realm();
    auto queue = move(endpoint.queue);
    endpoint.queued_bytes = 0;
    Vector<JS::Value> items;
    items.ensure_capacity(queue.size());
    for (auto& message : queue) {
        auto json = JS::PrimitiveString::create(vm, Utf16String::from_utf8(message.json));
        if (message.is_resolve) {
            items.append(JS::Array::create_from(realm, { JS::Value(1), JS::Value(static_cast<double>(message.call_id)), JS::Value(message.success), JS::Value(json) }));
        } else {
            auto name = JS::PrimitiveString::create(vm, Utf16String::from_utf8(message.name));
            items.append(JS::Array::create_from(realm, { JS::Value(0), JS::Value(name), JS::Value(json) }));
        }
    }
    return JS::Array::create_from(realm, items.span());
}

bool EngineChannel::can_enqueue(Endpoint const& endpoint, size_t bytes) const
{
    return endpoint.queue.size() < max_queued_messages && endpoint.queued_bytes + bytes <= max_queued_bytes;
}

void EngineChannel::enqueue(Endpoint& endpoint, Message&& message)
{
    endpoint.queued_bytes += message.name.length() + message.json.length();
    endpoint.queue.append(move(message));
    // Inside this endpoint's host callback its script is running: the page flushes at its next
    // microtask checkpoint. Any other endpoint gets a task.
    if (m_calling_endpoint != endpoint.id)
        schedule_task(endpoint);
}

void EngineChannel::schedule_task(Endpoint& endpoint)
{
    if (endpoint.task_scheduled)
        return;
    endpoint.task_scheduled = true;
    auto alive = m_alive;
    auto id = endpoint.id;
    auto& heap = endpoint.realm->heap();
    Web::HTML::queue_global_task(Web::HTML::Task::Source::PostedMessage, *endpoint.global, GC::create_function(heap, [this, alive, id] {
        auto* endpoint = alive->value ? this->endpoint(id) : nullptr;
        if (!endpoint)
            return;
        endpoint->task_scheduled = false;
        if (endpoint->queue.is_empty())
            return;
        auto& realm = *endpoint->realm;
        auto flush = endpoint->flush;
        Web::HTML::TemporaryExecutionContext context(realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto result = JS::call(realm.vm(), *flush, JS::js_undefined());
        if (result.is_error())
            Web::HTML::report_exception(result, realm);
    }));
}

kr_result EngineChannel::host_trigger(char const* name, char const* json_args)
{
    if (!name)
        return KR_ERROR_INVALID_ARGUMENT;
    auto name_length = strlen(name);
    if (name_length == 0 || name_length > max_name_bytes)
        return KR_ERROR_INVALID_ARGUMENT;
    if (m_endpoints.is_empty())
        return KR_ERROR_INVALID_STATE;
    // The same per-message ceiling as the page side; checked before copying anything.
    auto json_length = json_args ? strnlen(json_args, max_json_bytes + 1) : 0;
    if (json_length > max_json_bytes)
        return KR_ERROR_LIMIT_EXCEEDED;

    // All or nothing: a trigger one endpoint cannot take reaches none.
    for (auto& endpoint : m_endpoints) {
        if (!can_enqueue(*endpoint, name_length + json_length))
            return KR_ERROR_LIMIT_EXCEEDED;
    }
    // ByteString shares its buffer, so each endpoint's copy is one allocation.
    auto name_string = ByteString(name, name_length);
    auto json_string = ByteString(json_args ? json_args : "", json_length);
    for (auto& endpoint : m_endpoints) {
        Message message;
        message.name = name_string;
        message.json = json_string;
        enqueue(*endpoint, move(message));
    }
    return KR_OK;
}

kr_result EngineChannel::host_resolve(uint64_t call_id, bool success, char const* json_value)
{
    auto owner = m_pending_calls.get(call_id);
    auto* endpoint = owner.has_value() ? this->endpoint(*owner) : nullptr;
    if (!endpoint)
        return KR_ERROR_INVALID_ARGUMENT;
    // The same per-message ceiling as host_trigger, checked before copying anything. A refused
    // resolve leaves the call pending, so the host can still answer it with a smaller value.
    auto json_length = json_value ? strnlen(json_value, max_json_bytes + 1) : 0;
    if (json_length > max_json_bytes || !can_enqueue(*endpoint, json_length))
        return KR_ERROR_LIMIT_EXCEEDED;
    Message message;
    message.is_resolve = true;
    message.call_id = call_id;
    message.success = success;
    message.json = json_value ? ByteString(json_value, json_length) : ByteString {};
    m_pending_calls.remove(call_id);
    --endpoint->pending_calls;
    enqueue(*endpoint, move(message));
    return KR_OK;
}

}
