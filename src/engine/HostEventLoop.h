/*
 * Copyright (c) 2026, the karouka authors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>

namespace Karouka {

// The host-driven event loop. karouka installs its own Core::EventLoopManager, so that the
// main thread's Core::EventLoop is serviced only when the host calls in (kr_view_advance_time pumps
// it) and on the host's clock:
// - Core::Timers of the main thread fire when the host clock (set_event_loop_time, the time LibWeb's
//   host clock is stepped to) reaches their deadline, not when the wall clock does. A zero-interval
//   one fires at the next pump, as before. With kr_system_desc.real_time_clock they keep the
//   system's monotonic clock.
// - A pump never sleeps and makes no system call: no wake pipe, no poll() unless a Core::Notifier
//   is registered on the main thread (then poll() with a zero timeout).
// - Deferred invocations and wakes from other threads (stage threads, the thread pool, a host that
//   completes a load) are queued and run by the next pump; a wake is a flag and a condition
//   variable, not a pipe write.
// Only a nested event loop (LibWeb spinning the loop until a condition, PumpMode::WaitForEvents)
// waits, for another thread's wake, as it did on poll() before; it cannot wait for the host clock,
// which stands still within a call.
// Threads other than the one that installed it keep LibCore's Unix event loop (poll, wake pipe,
// wall-clock timers): their loops block in their own exec(), which the host never drives.
//
// Installed once per process, before the first Core::EventLoop (kr_system_create2); it outlives the
// system, like the rest of LibWeb's process-wide state.
void install_host_event_loop(bool real_time_clock, double start_ms);

// Moves the main thread's timer clock to now_ms (never back). Due timers fire at the next pump.
void set_event_loop_time(double now_ms);

// Whether a main-thread Core::Timer with a nonzero interval is due at the clock's time, so that the
// next pump fires it. Zero-interval timers are always due and fire at whichever pump comes next.
bool event_loop_has_due_timers();

// The earliest deadline after the clock's time of a main-thread Core::Timer with a nonzero
// interval, if any: the host clock must stop there for the timer to fire on time.
Optional<double> event_loop_next_timer_deadline();

// Main-thread Core::Timer firings so far (KR_DEBUG's counters line).
u64 event_loop_timers_fired();

// Main-thread event loop pumps so far (KR_DEBUG's counters line).
u64 event_loop_pumps();

}
