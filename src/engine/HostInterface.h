/*
 * The host interface (kr_host_interface, karouka.h) inside the engine: the clock, the log and the
 * io routing. Process-wide, like the system it is installed with.
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <karouka/karouka.h>

namespace JS {
class Realm;
}

namespace Web {
class FileRequest;
}

namespace Karouka {

// Whether kr_system_create2 can take this host interface: NULL (every default), or a struct_size
// that holds io (StructSize.h). Checked before anything process-wide starts.
bool host_interface_is_valid(kr_host_interface const*);

// Copies a valid host interface given to kr_system_create2: the members that end within its
// struct_size (StructSize.h); the rest, or all of them for NULL, zero. verbose is
// kr_system_desc.verbose.
void install_host_interface(kr_host_interface const*, bool verbose);
void uninstall_host_interface();

// ---- clock

// The host's monotonic clock in nanoseconds, or the system's without one. Plain function and
// user data so a LibWeb embedder clock hook can take it as is.
struct HostClock {
    u64 (*monotonic_ns)(void* user_data) { nullptr };
    void* user_data { nullptr };
};
HostClock host_clock();
u64 host_monotonic_ns();
double host_now_ms();

// ---- log

// KR_DEBUG is set (read once). Gates karouka's DEBUG lines that cost work to gather, such as the
// per-frame ones, whether or not the host has a log.
bool debug_enabled();

// Whether a "karouka" line at `level` is delivered: with a host log, at or above its min_level;
// without one (stderr), at INFO and above, and DEBUG and TRACE too when debug_enabled().
bool log_enabled(kr_log_level);
void log_line(kr_log_level, StringView category, StringView message);

// karouka's own diagnostics: category "karouka". Formats only if a line at `level` is delivered.
template<typename... Parameters>
void log(kr_log_level level, CheckedFormatString<Parameters...>&& format, Parameters const&... parameters)
{
    if (!log_enabled(level))
        return;
    AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    log_line(level, "karouka"sv, ByteString::vformatted(format.view(), variadic_format_params));
}

// Routes the realm's console.* output and uncaught exceptions to the host log ("console").
void install_console_client(JS::Realm&);

// ---- io

class HostIo {
public:
    // Whether the host routes file loads (kr_host_io.request is set).
    static bool is_routed();

    // Called with the outcome of a load; its return value is what request() returns when the
    // outcome arrived inside the host's request call.
    using Completion = Function<kr_result(kr_result status, ReadonlyBytes data)>;

    // Starts a load of `url` (a file: URL) through the host. `owner` groups loads for cancel_all.
    // Returns the completion's result if the host completed inside request, or empty when pending.
    static Optional<kr_result> request(ByteString const& url, void const* owner, Completion);

    // Answers a FileRequest from LibWeb's ResourceLoader through the host; LibWeb wants a readable
    // fd, so the bytes are fed to it through a pipe.
    static void request_file(Web::FileRequest, void const* owner);

    static kr_result complete(u64 id, kr_result status, void const* data, size_t size, kr_io_ownership);

    // Drops every pending load of `owner` (nullptr: every load) and tells the host to cancel them.
    static void cancel_all(void const* owner);
};

}
