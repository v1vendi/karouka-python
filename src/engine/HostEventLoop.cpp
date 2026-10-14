/*
 * Copyright (c) 2026, the karouka authors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HostEventLoop.h"

#include <AK/QuickSort.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibCore/EventLoopImplementationUnix.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/Platform/ScopedAutoreleasePool.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/ThreadPool.h>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <pthread.h>

namespace Karouka {

namespace {

// A main-thread Core::Timer. Its id is its address with the low bit set, which tells it from the
// Unix manager's (aligned EventLoopTimer addresses) when it is unregistered.
struct HostTimer {
    WeakPtr<Core::EventReceiver> owner;
    double interval_ms { 0 };
    double due_ms { 0 };
    u64 sequence { 0 };
    bool should_reload { false };
};

class HostEventLoopManager;

// The main thread's loop: a pump fires the due timers, polls the notifiers without waiting, and runs
// the thread's queued events (timer and notifier events, deferred invocations).
class HostEventLoopImplementation final : public Core::EventLoopImplementation {
public:
    explicit HostEventLoopImplementation(HostEventLoopManager& manager)
        : m_manager(manager)
    {
    }

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int code) override;
    virtual void wake() override;

private:
    HostEventLoopManager& m_manager;
};

// Any other thread's loop: LibCore's Unix loop, unchanged. EventLoopImplementationUnix::pump reaches
// its manager through EventLoopManager::the(), which is karouka's now, so this pumps the Unix manager
// karouka keeps for these threads itself, and borrows the Unix implementation's wake pipe.
class ThreadEventLoopImplementation final : public Core::EventLoopImplementation {
public:
    explicit ThreadEventLoopImplementation(Core::EventLoopManagerUnix& manager)
        : m_manager(manager)
        , m_waker(Core::EventLoopImplementationUnix::create())
    {
    }

    virtual int exec() override
    {
        for (;;) {
            if (auto exit_code = exit_code_if_requested(); exit_code.has_value())
                return exit_code.release_value();
            pump(PumpMode::WaitForEvents);
        }
    }

    virtual size_t pump(PumpMode mode) override
    {
        Core::ScopedAutoreleasePool autorelease_pool;
        m_manager.wait_for_events(mode);
        return m_thread_event_queue.process();
    }

    virtual void quit(int code) override { request_exit(code); }
    virtual void wake() override { m_waker->wake(); }

private:
    Core::EventLoopManagerUnix& m_manager;
    NonnullOwnPtr<Core::EventLoopImplementationUnix> m_waker;
};

class HostEventLoopManager final : public Core::EventLoopManager {
public:
    HostEventLoopManager(bool real_time_clock, double start_ms)
        : m_main_thread(pthread_self())
        , m_real_time_clock(real_time_clock)
        , m_time_ms(start_ms)
    {
    }

    bool is_main_thread() const { return pthread_equal(pthread_self(), m_main_thread); }

    virtual NonnullOwnPtr<Core::EventLoopImplementation> make_implementation() override
    {
        if (is_main_thread())
            return adopt_own(*new HostEventLoopImplementation(*this));
        return adopt_own(*new ThreadEventLoopImplementation(m_other_threads));
    }

    virtual intptr_t register_timer(Core::EventReceiver& object, int milliseconds, bool should_reload) override
    {
        if (!is_main_thread())
            return m_other_threads.register_timer(object, milliseconds, should_reload);
        VERIFY(milliseconds >= 0);
        auto* timer = new HostTimer;
        timer->owner = object;
        timer->interval_ms = milliseconds;
        timer->should_reload = should_reload;
        std::lock_guard locker(m_mutex);
        timer->due_ms = now_ms() + milliseconds;
        timer->sequence = m_next_sequence++;
        m_timers.append(timer);
        return bit_cast<intptr_t>(timer) | 1;
    }

    virtual void unregister_timer(intptr_t timer_id) override
    {
        if (!(timer_id & 1)) {
            m_other_threads.unregister_timer(timer_id);
            return;
        }
        auto* timer = bit_cast<HostTimer*>(timer_id & ~static_cast<intptr_t>(1));
        {
            std::lock_guard locker(m_mutex);
            m_timers.remove_first_matching([&](auto* each) { return each == timer; });
        }
        delete timer;
    }

    virtual void register_notifier(Core::Notifier& notifier) override
    {
        if (!is_main_thread()) {
            m_other_threads.register_notifier(notifier);
            return;
        }
        notifier.set_owner_thread(m_main_thread);
        std::lock_guard locker(m_mutex);
        m_notifiers.append(&notifier);
    }

    virtual void unregister_notifier(Core::Notifier& notifier) override
    {
        if (!pthread_equal(notifier.owner_thread(), m_main_thread)) {
            m_other_threads.unregister_notifier(notifier);
            return;
        }
        std::lock_guard locker(m_mutex);
        m_notifiers.remove_first_matching([&](auto* each) { return each == &notifier; });
    }

    // The queue's owner is woken by EventLoopImplementation::deferred_invoke (or an explicit wake).
    virtual void did_post_event() override { }

    // Process-wide; LibCore dispatches them from the Unix loop of the thread that registered first.
    // Nothing in karouka registers one (the main thread's loop never polls the Unix wake pipe).
    virtual int register_signal(int signal_number, Function<void(int)> handler) override
    {
        return m_other_threads.register_signal(signal_number, move(handler));
    }
    virtual void unregister_signal(int handler_id) override { m_other_threads.unregister_signal(handler_id); }

    u64 timers_fired()
    {
        std::lock_guard locker(m_mutex);
        return m_timers_fired;
    }

    u64 pumps() const { return m_pumps; }

    // The earliest deadline after the clock's time of a timer with a nonzero interval. A zero-interval
    // timer is due at every pump, and one that fired once and was not restarted is never due again.
    Optional<double> next_deadline()
    {
        std::lock_guard locker(m_mutex);
        auto now = now_ms();
        Optional<double> next;
        for (auto* timer : m_timers) {
            if (timer->interval_ms <= 0 || timer->due_ms <= now || timer->due_ms == AK::NumericLimits<double>::max())
                continue;
            if (!next.has_value() || timer->due_ms < *next)
                next = timer->due_ms;
        }
        return next;
    }

    void set_time(double now_ms)
    {
        std::lock_guard locker(m_mutex);
        m_time_ms = max(m_time_ms, now_ms);
    }

    bool has_due_timers()
    {
        std::lock_guard locker(m_mutex);
        auto now = now_ms();
        for (auto* timer : m_timers) {
            if (timer->interval_ms > 0 && timer->due_ms <= now)
                return true;
        }
        return false;
    }

    // One pump's servicing, before the queue runs.
    void service(Core::EventLoopImplementation::PumpMode mode)
    {
        ++m_pumps;
        // On the host clock, the pool's awaitable work (script compiles, stylesheet parses, web font
        // preparation; patch 0051) finishes before a pump, which then runs what it posted back: a fetched
        // script runs at the same pump, so the same frame, on every run, as one parsed inline would.
        // Previously, its result came back on the wall clock, and demo/ranger_sector's 32 KB
        // script ran anywhere from frame 31 to frame 69 headless, by machine load. The main thread
        // waits out the compile instead (a few ms for a large bundle, once), as Tessera, which
        // compiles on the main thread, does. The real-time clock keeps the frame going meanwhile.
        if (!m_real_time_clock)
            Threading::ThreadPool::wait_for_awaitable_work();
        consume_wake();
        poll_notifiers();
        fire_due_timers();
        if (mode != Core::EventLoopImplementation::PumpMode::WaitForEvents || Core::ThreadEventQueue::current().has_pending_events())
            return;
        // A nested loop with nothing to run waits for another thread's wake, or (on the real-time
        // clock) for the next timer; with a notifier registered, it polls it every millisecond.
        wait_for_wake();
        poll_notifiers();
        fire_due_timers();
    }

    void wake()
    {
        {
            std::lock_guard locker(m_wake_mutex);
            m_wake_requested = true;
        }
        m_wake_condition.notify_one();
    }

private:
    double now_ms() const
    {
        if (m_real_time_clock)
            return static_cast<double>(MonotonicTime::now().nanoseconds()) / 1'000'000.0;
        return m_time_ms;
    }

    void consume_wake()
    {
        std::lock_guard locker(m_wake_mutex);
        m_wake_requested = false;
    }

    void wait_for_wake()
    {
        Optional<double> wait_ms;
        {
            std::lock_guard locker(m_mutex);
            if (!m_notifiers.is_empty())
                wait_ms = 1;
            if (m_real_time_clock) {
                auto now = now_ms();
                for (auto* timer : m_timers) {
                    if (timer->due_ms == AK::NumericLimits<double>::max())
                        continue;
                    auto until = max(timer->due_ms - now, 0.0);
                    if (!wait_ms.has_value() || until < *wait_ms)
                        wait_ms = until;
                }
            }
        }
        std::unique_lock locker(m_wake_mutex);
        if (wait_ms.has_value()) {
            m_wake_condition.wait_for(locker, std::chrono::microseconds(static_cast<i64>(*wait_ms * 1000)), [&] { return m_wake_requested; });
        } else {
            m_wake_condition.wait(locker, [&] { return m_wake_requested; });
        }
        m_wake_requested = false;
    }

    // Fires each timer due at the clock's time once, earliest deadline first, then registration
    // order: it posts the timer event the pump's queue run delivers. A reloading timer keeps to its
    // schedule and skips the firings it fell behind on, as LibCore's does.
    void fire_due_timers()
    {
        Vector<NonnullRefPtr<Core::EventReceiver>, 8> owners;
        {
            std::lock_guard locker(m_mutex);
            if (m_timers.is_empty())
                return;
            auto now = now_ms();
            Vector<HostTimer*, 8> due;
            for (auto* timer : m_timers) {
                if (timer->due_ms <= now)
                    due.append(timer);
            }
            if (due.is_empty())
                return;
            quick_sort(due, [](HostTimer const* a, HostTimer const* b) {
                return a->due_ms < b->due_ms || (a->due_ms == b->due_ms && a->sequence < b->sequence);
            });
            for (auto* timer : due) {
                auto owner = timer->owner.strong_ref();
                if (timer->should_reload) {
                    timer->due_ms += timer->interval_ms;
                    if (timer->due_ms <= now && timer->interval_ms > 0)
                        timer->due_ms = now + timer->interval_ms;
                } else {
                    // Fired once; it stays registered, never due again, until it is unregistered.
                    timer->due_ms = AK::NumericLimits<double>::max();
                }
                if (owner)
                    owners.append(owner.release_nonnull());
                ++m_timers_fired;
            }
        }
        auto& queue = Core::ThreadEventQueue::current();
        for (auto& owner : owners)
            queue.post_event(owner.ptr(), Core::Event::Type::Timer);
    }

    void poll_notifiers()
    {
        Vector<Core::Notifier*, 4> notifiers;
        Vector<pollfd, 4> fds;
        {
            std::lock_guard locker(m_mutex);
            if (m_notifiers.is_empty())
                return;
            for (auto* notifier : m_notifiers) {
                short events = 0;
                if (has_flag(notifier->type(), Core::NotificationType::Read))
                    events |= POLLIN;
                if (has_flag(notifier->type(), Core::NotificationType::Write))
                    events |= POLLOUT;
                notifiers.append(notifier);
                fds.append({ .fd = notifier->fd(), .events = events, .revents = 0 });
            }
        }
        if (::poll(fds.data(), fds.size(), 0) <= 0)
            return;
        auto& queue = Core::ThreadEventQueue::current();
        for (size_t i = 0; i < fds.size(); ++i) {
            auto revents = fds[i].revents;
            auto type = Core::NotificationType::None;
            if (revents & POLLIN)
                type |= Core::NotificationType::Read;
            if (revents & POLLOUT)
                type |= Core::NotificationType::Write;
            if (revents & POLLHUP)
                type |= Core::NotificationType::Read | Core::NotificationType::Write | Core::NotificationType::HangUp;
            if (revents & POLLERR)
                type |= Core::NotificationType::Read | Core::NotificationType::Write | Core::NotificationType::Error;
            type &= notifiers[i]->type();
            if (type != Core::NotificationType::None)
                queue.post_event(notifiers[i], Core::Event::Type::NotifierActivation);
        }
    }

    pthread_t m_main_thread;
    bool m_real_time_clock { false };
    Core::EventLoopManagerUnix m_other_threads;

    std::mutex m_mutex;
    double m_time_ms { 0 };
    u64 m_next_sequence { 0 };
    u64 m_timers_fired { 0 };
    u64 m_pumps { 0 }; // Main thread only.
    Vector<HostTimer*> m_timers;
    Vector<Core::Notifier*> m_notifiers;

    std::mutex m_wake_mutex;
    std::condition_variable m_wake_condition;
    bool m_wake_requested { false };
};

HostEventLoopManager* s_manager;

int HostEventLoopImplementation::exec()
{
    for (;;) {
        if (auto exit_code = exit_code_if_requested(); exit_code.has_value())
            return exit_code.release_value();
        pump(PumpMode::WaitForEvents);
    }
}

size_t HostEventLoopImplementation::pump(PumpMode mode)
{
    Core::ScopedAutoreleasePool autorelease_pool;
    m_manager.service(mode);
    return m_thread_event_queue.process();
}

void HostEventLoopImplementation::quit(int code)
{
    request_exit(code);
    m_manager.wake();
}

void HostEventLoopImplementation::wake()
{
    m_manager.wake();
}

}

void install_host_event_loop(bool real_time_clock, double start_ms)
{
    VERIFY(!s_manager);
    // Never destroyed: timers and notifiers unregister themselves during exit()'s static destructors.
    s_manager = new HostEventLoopManager(real_time_clock, start_ms);
    Core::EventLoopManager::install(*s_manager);
}

void set_event_loop_time(double now_ms)
{
    if (s_manager)
        s_manager->set_time(now_ms);
}

bool event_loop_has_due_timers()
{
    return s_manager && s_manager->has_due_timers();
}

u64 event_loop_timers_fired()
{
    return s_manager ? s_manager->timers_fired() : 0;
}

Optional<double> event_loop_next_timer_deadline()
{
    return s_manager ? s_manager->next_deadline() : Optional<double> {};
}

u64 event_loop_pumps()
{
    return s_manager ? s_manager->pumps() : 0;
}

}
