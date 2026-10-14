/*
 * The host interface inside the engine: the host's clock, its log (karouka's diagnostics and the
 * pages' console) and its io (file: loads). See kr_host_interface in karouka.h for the contract
 * and for what each seam does not reach yet.
 */

#include "HostInterface.h"
#include "StructSize.h"

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/FileRequest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <thread>
#include <unistd.h>

namespace Karouka {

namespace {

struct Host {
    kr_host_io io {};
    kr_host_clock clock {};
    kr_host_log log {};
    bool verbose { false };
};

Host s_host;

// The members after struct_size, in order (StructSize.h).
constexpr StructMember host_interface_members[] = {
    KR_STRUCT_MEMBER(kr_host_interface, io),
    KR_STRUCT_MEMBER(kr_host_interface, clock),
    KR_STRUCT_MEMBER(kr_host_interface, log),
};
static_assert(lists_every_member<kr_host_interface>(host_interface_members));

}

bool host_interface_is_valid(kr_host_interface const* host)
{
    return !host || whole_members_end(host->struct_size, host_interface_members) != 0;
}

void install_host_interface(kr_host_interface const* host, bool verbose)
{
    s_host = {};
    s_host.verbose = verbose;
    if (!host)
        return;
    // The members past a short struct_size are zero, their defaults. One that can't hold io was
    // refused by kr_system_create2 (host_interface_is_valid).
    kr_host_interface copy {};
    VERIFY(read_sized(*host, host_interface_members, copy));
    s_host.io = copy.io;
    s_host.clock = copy.clock;
    s_host.log = copy.log;
}

void uninstall_host_interface()
{
    s_host = {};
}

// ---- clock

HostClock host_clock()
{
    return { s_host.clock.monotonic_ns, s_host.clock.user_data };
}

u64 host_monotonic_ns()
{
    if (s_host.clock.monotonic_ns)
        return s_host.clock.monotonic_ns(s_host.clock.user_data);
    return static_cast<u64>(MonotonicTime::now().nanoseconds());
}

double host_now_ms()
{
    return static_cast<double>(host_monotonic_ns()) / 1'000'000.0;
}

// ---- log

bool debug_enabled()
{
    static bool const enabled = getenv("KR_DEBUG") != nullptr;
    return enabled;
}

bool log_enabled(kr_log_level level)
{
    if (s_host.log.write)
        return level >= s_host.log.min_level;
    return level >= KR_LOG_INFO || debug_enabled();
}

// Without a host log, console output is printed only when the system is verbose.
static bool console_log_enabled(kr_log_level level)
{
    return s_host.log.write ? level >= s_host.log.min_level : s_host.verbose;
}

static StringView level_name(kr_log_level level)
{
    switch (level) {
    case KR_LOG_TRACE:
        return "trace"sv;
    case KR_LOG_DEBUG:
        return "debug"sv;
    case KR_LOG_INFO:
        return "info"sv;
    case KR_LOG_WARN:
        return "warn"sv;
    case KR_LOG_ERROR:
        return "error"sv;
    }
    return "?"sv;
}

void log_line(kr_log_level level, StringView category, StringView message)
{
    if (!s_host.log.write) {
        if (category == "karouka"sv)
            dbgln("karouka: {}", message);
        else
            dbgln("{} {}: {}", category, level_name(level), message);
        return;
    }
    if (level < s_host.log.min_level)
        return;
    // NUL-terminated copies: the views a caller passes need not be.
    auto category_string = ByteString(category);
    auto message_string = ByteString(message);
    s_host.log.write(s_host.log.user_data, level, category_string.characters(), message_string.characters());
}

namespace {

// A document's console, as WebContent's console client would print it, to the host log.
class ConsoleClient final : public JS::ConsoleClient {
    GC_CELL(ConsoleClient, JS::ConsoleClient);
    GC_DECLARE_ALLOCATOR(ConsoleClient);

public:
    static GC::Ref<ConsoleClient> create(JS::Console& console) { return GC::Heap::the().allocate<ConsoleClient>(console); }

    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel console_level, PrinterArguments arguments) override
    {
        auto level = level_of(console_level);
        if (!console_log_enabled(level))
            return JS::js_undefined();
        // Table is not laid out; its data prints like console.log's.
        if (auto* trace = arguments.get_pointer<JS::Console::Trace>()) {
            StringBuilder builder;
            builder.append(trace->label.to_byte_string());
            for (auto const& frame : trace->stack) {
                auto function_name = frame.function_name.to_byte_string();
                builder.appendff("\n    at {}", function_name.is_empty() ? "<anonymous>"sv : function_name.view());
                if (frame.source_file.has_value())
                    builder.appendff(" ({}:{}:{})", frame.source_file->to_byte_string(), frame.line.value_or(0), frame.column.value_or(0));
            }
            log_line(level, "console"sv, builder.string_view());
        } else if (auto* group = arguments.get_pointer<JS::Console::Group>()) {
            log_line(level, "console"sv, group->label.to_byte_string());
        } else {
            // Strings as they are, like a browser's console shows them; other values as LibJS
            // prints them (strings nested in them quoted).
            StringBuilder builder;
            bool first = true;
            for (auto value : arguments.get<GC::RootVector<JS::Value>>()) {
                if (!exchange(first, false))
                    builder.append(' ');
                if (value.is_string()) {
                    builder.append(value.as_string().utf16_string().to_byte_string());
                    continue;
                }
                GC::RootVector<JS::Value> single { ReadonlySpan<JS::Value> { &value, 1 } };
                builder.append(TRY(generically_format_values(single)).to_byte_string());
            }
            log_line(level, "console"sv, builder.string_view());
        }
        return JS::js_undefined();
    }

    virtual void report_exception(Utf16View name, Utf16View message, JS::ErrorData const&, bool in_promise) override
    {
        if (!console_log_enabled(KR_LOG_ERROR))
            return;
        log_line(KR_LOG_ERROR, "console"sv, ByteString::formatted("Uncaught{} {}: {}", in_promise ? " (in promise)"sv : ""sv, name, message));
    }

    virtual void clear() override { }
    virtual void end_group() override { }

private:
    explicit ConsoleClient(JS::Console& console)
        : JS::ConsoleClient(console)
    {
    }

    static kr_log_level level_of(JS::Console::LogLevel level)
    {
        switch (level) {
        case JS::Console::LogLevel::Debug:
        case JS::Console::LogLevel::Trace:
            return KR_LOG_DEBUG;
        case JS::Console::LogLevel::Warn:
            return KR_LOG_WARN;
        case JS::Console::LogLevel::Error:
        case JS::Console::LogLevel::Assert:
            return KR_LOG_ERROR;
        default:
            return KR_LOG_INFO;
        }
    }
};

GC_DEFINE_ALLOCATOR(ConsoleClient);

}

void install_console_client(JS::Realm& realm)
{
    // Without a destination the console stays without a client, as before: console.* calls then
    // format nothing.
    if (!s_host.log.write && !s_host.verbose)
        return;
    auto& console = realm.intrinsics().console_object()->console();
    console.set_client(*ConsoleClient::create(console));
}

// ---- io

namespace {

struct PendingLoad {
    void const* owner { nullptr };
    HostIo::Completion completion;
    // kr_io_complete has delivered it; its completion runs in a deferred task.
    bool completed { false };
};

HashMap<u64, PendingLoad> s_pending;
u64 s_next_request_id { 1 };
// The load whose kr_host_io.request call is on the stack, and its completion's result once the
// host completed it from inside that call.
Optional<u64> s_inline_id;
Optional<kr_result> s_inline_result;

}

bool HostIo::is_routed()
{
    return s_host.io.request != nullptr;
}

Optional<kr_result> HostIo::request(ByteString const& url, void const* owner, Completion completion)
{
    VERIFY(is_routed());
    auto id = s_next_request_id++;
    s_pending.set(id, PendingLoad { owner, move(completion), false });

    // A completion may start loads of its own; each request call keeps its own inline state.
    auto outer_id = s_inline_id;
    auto outer_result = s_inline_result;
    s_inline_id = id;
    s_inline_result.clear();
    auto status = s_host.io.request(s_host.io.user_data, url.characters(), id);
    auto inline_result = s_inline_result;
    s_inline_id = outer_id;
    s_inline_result = outer_result;

    if (inline_result.has_value())
        return inline_result;
    if (status != KR_OK) {
        // Refused without a completion. A completion delivered for later (completed) still wins.
        auto it = s_pending.find(id);
        if (it == s_pending.end() || it->value.completed)
            return {};
        auto load = s_pending.take(id).release_value();
        return load.completion(status, {});
    }
    return {};
}

kr_result HostIo::complete(u64 id, kr_result status, void const* data, size_t size, kr_io_ownership ownership)
{
    if (!data && size)
        return KR_ERROR_INVALID_ARGUMENT;
    auto it = s_pending.find(id);
    if (it == s_pending.end() || it->value.completed)
        return KR_ERROR_INVALID_ARGUMENT;
    auto release = [&] {
        if (ownership == KR_IO_OWNERSHIP_BORROW_UNTIL_RELEASE && s_host.io.release)
            s_host.io.release(s_host.io.user_data, id, data);
    };
    ReadonlyBytes bytes { static_cast<u8 const*>(data), status == KR_OK ? size : 0 };

    // From inside the request call: the load is acted on at once, from the host's buffer.
    if (s_inline_id == id) {
        auto load = s_pending.take(id).release_value();
        s_inline_result = load.completion(status, bytes);
        release();
        return KR_OK;
    }

    // Later: copied, and acted on by a task, so the host may complete from anywhere on the thread
    // that drives the system without running page work under its own call.
    it->value.completed = true;
    auto copy = ByteBuffer::copy(bytes);
    release();
    if (copy.is_error())
        status = KR_ERROR_INTERNAL;
    Core::deferred_invoke([id, status, copy = copy.is_error() ? ByteBuffer {} : copy.release_value()] {
        auto load = s_pending.take(id);
        if (!load.has_value())
            return; // cancelled meanwhile
        (void)load->completion(status, copy.bytes());
    });
    return KR_OK;
}

void HostIo::cancel_all(void const* owner)
{
    Vector<u64> ids;
    for (auto const& [id, load] : s_pending) {
        if (!owner || load.owner == owner)
            ids.append(id);
    }
    for (auto id : ids) {
        auto load = s_pending.take(id).release_value();
        if (!load.completed && s_host.io.cancel)
            s_host.io.cancel(s_host.io.user_data, id);
    }
}

// A readable fd that yields `data` then EOF, for LibWeb's FileRequest, which takes an fd (the
// WebContent process gets one from the UI process). What fits in the pipe's buffer is written now;
// the rest by a thread of its own, which ends when the reader has read it all or closed its end.
static ErrorOr<int> pipe_with_contents(ReadonlyBytes data)
{
    int fds[2];
    if (pipe(fds) < 0)
        return Error::from_errno(errno);
    auto fail = [&](int error) {
        close(fds[0]);
        close(fds[1]);
        return Error::from_errno(error);
    };
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)
        return fail(errno);
    // A reader that gives up early must cost the writer an EPIPE, not the process a SIGPIPE.
    if (fcntl(fds[1], F_SETNOSIGPIPE, 1) < 0)
        return fail(errno);
    auto flags = fcntl(fds[1], F_GETFL);
    if (flags < 0 || fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) < 0)
        return fail(errno);

    size_t written = 0;
    while (written < data.size()) {
        auto n = write(fds[1], data.data() + written, data.size() - written);
        if (n > 0) {
            written += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN)
            return fail(errno);
        break;
    }
    if (written == data.size()) {
        close(fds[1]);
        return fds[0];
    }

    auto rest = ByteBuffer::copy(data.slice(written));
    if (rest.is_error() || fcntl(fds[1], F_SETFL, flags) < 0)
        return fail(ENOMEM);
    std::thread([fd = fds[1], rest = rest.release_value()] {
        size_t offset = 0;
        while (offset < rest.size()) {
            auto n = write(fd, rest.data() + offset, rest.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            offset += n;
        }
        close(fd);
    }).detach();
    return fds[0];
}

void HostIo::request_file(Web::FileRequest file_request, void const* owner)
{
    auto finish = move(file_request.on_file_request_finish);
    auto url = URL::create_with_file_scheme(file_request.path());
    if (!url.has_value()) {
        finish(Error::from_errno(EINVAL));
        return;
    }
    // The query isn't the file's, but the host may act on it (wptserve's `?pipe=` in the WPT viewer).
    if (auto const& query = file_request.query(); query.has_value())
        url->set_query(query->view());
    (void)request(url->serialize().to_byte_string(), owner, [finish = move(finish)](kr_result status, ReadonlyBytes data) mutable {
        if (status != KR_OK) {
            finish(Error::from_errno(status == KR_ERROR_IO ? ENOENT : EIO));
            return status;
        }
        auto fd = pipe_with_contents(data);
        auto result = fd.is_error() ? KR_ERROR_IO : KR_OK;
        finish(move(fd));
        return result;
    });
}

}
