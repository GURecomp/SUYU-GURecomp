// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "common/cpu_features.h"
#include "common/cpu_features.h"

#ifdef _WIN32
#include "common/windows/timer_resolution.h"
#endif

#include "common/logging/log.h"
#include "common/settings.h"
#include "core/arm/recomp/game_settings.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"

namespace Core::Timing {

constexpr s64 MAX_SLICE_LENGTH = 10000;

namespace {
// Diagnostics for the host timing thread (game_settings.ini [Debug] diagnostics): how long its
// waits are and which events wake it, logged every 30 s. Only the timing thread touches it.
struct TimingStats {
    static constexpr std::array<s64, 6> kEdgesNs{250'000, 500'000, 1'000'000, 2'000'000,
                                                 3'000'000, 5'000'000};
    std::array<u64, kEdgesNs.size() + 1> waits{};
    std::array<s64, kEdgesNs.size() + 1> wait_ns{};
    u64 idle_waits = 0;
    std::unordered_map<const EventType*, std::pair<std::string, u64>> fired;
    s64 last_flush = 0;

    void Wait(s64 ns) {
        size_t i = 0;
        while (i < kEdgesNs.size() && ns >= kEdgesNs[i]) {
            ++i;
        }
        ++waits[i];
        wait_ns[i] += ns;
    }
    void Fired(const EventType* type) {
        auto& entry = fired[type];
        if (entry.second++ == 0) {
            entry.first = type->name;
        }
    }
    void MaybeFlush(s64 now) {
        if (last_flush == 0) {
            last_flush = now;
            return;
        }
        const s64 span = now - last_flush;
        if (span < 30'000'000'000) {
            return;
        }
        static constexpr std::array<const char*, kEdgesNs.size() + 1> kNames{
            "<0.25ms", "<0.5ms", "<1ms", "<2ms", "<3ms", "<5ms", ">=5ms"};
        std::string line;
        for (size_t i = 0; i < waits.size(); ++i) {
            line += fmt::format(" {} {} ({:.1f}%)", kNames[i], waits[i],
                                100.0 * static_cast<double>(wait_ns[i]) /
                                    static_cast<double>(span));
        }
        LOG_INFO(Core_Timing, "timing thread over {:.1f} s: waits{}; idle {}",
                 static_cast<double>(span) / 1e9, line, idle_waits);
        std::vector<std::pair<std::string, u64>> top;
        for (auto& [type, entry] : fired) {
            top.push_back(entry);
        }
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string events;
        for (size_t i = 0; i < top.size() && i < 10; ++i) {
            events += fmt::format(" {} {}/s;", top[i].first,
                                  static_cast<double>(top[i].second) * 1e9 /
                                      static_cast<double>(span));
        }
        LOG_INFO(Core_Timing, "timing thread events:{}", events);
        waits.fill(0);
        wait_ns.fill(0);
        idle_waits = 0;
        fired.clear();
        last_flush = now;
    }
};
thread_local TimingStats* t_stats = nullptr;
} // namespace

std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback) {
    return std::make_shared<EventType>(std::move(callback), std::move(name));
}

struct CoreTiming::Event {
    s64 time;
    u64 fifo_order;
    std::weak_ptr<EventType> type;
    s64 reschedule_time;
    heap_t::handle_type handle{};

    // Sort by time, unless the times are the same, in which case sort by
    // the order added to the queue
    friend bool operator>(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) > std::tie(right.time, right.fifo_order);
    }

    friend bool operator<(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) < std::tie(right.time, right.fifo_order);
    }
};

CoreTiming::CoreTiming() = default;
CoreTiming::~CoreTiming() {
    Reset();
}

void CoreTiming::Initialize(std::function<void()>&& on_thread_init_) {
    Reset();
    on_thread_init = std::move(on_thread_init_);
    event_fifo_id = 0;
    cpu_ticks = 0;
    if (is_multicore) {
        timer_thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("HostTiming");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
            on_thread_init();
            has_started = true;
            TimingStats stats;
            if (GameSettings::DiagnosticsEnabled()) {
                t_stats = &stats;
            }

            // base frequency in MHz: 1ns (10^-9) = 1GHz (10^9)
            while (!stop_token.stop_requested()) {
                while (!paused && !stop_token.stop_requested()) {
                    paused_set = false;
                    if (auto const next_time = Advance(); next_time) {
                        // There are more events left in the queue, wait until the next event.
                        auto const wait_time = *next_time - GetGlobalTimeNs().count();
                        if (wait_time > 0) {
                            if (t_stats) {
                                t_stats->Wait(wait_time);
                            }
                            event.WaitFor(std::chrono::nanoseconds(wait_time));
                        }
                        if (t_stats) {
                            t_stats->MaybeFlush(GetGlobalTimeNs().count());
                        }
                    } else {
                        // Queue is empty, wait until another event is scheduled and signals us to
                        // continue.
                        wait_set = true;
                        if (t_stats) {
                            ++t_stats->idle_waits;
                        }
                        event.Wait();
                    }
                    wait_set = false;
                }
                paused_set = true;
                pause_event.Wait();
            }
        });
    }
}

void CoreTiming::ClearPendingEvents() {
    std::scoped_lock lock{advance_lock, basic_lock};
    event_queue.clear();
    event.Set();
}

void CoreTiming::Pause(bool is_paused) {
    paused = is_paused;
    pause_event.Set();

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

void CoreTiming::SyncPause(bool is_paused) {
    if (is_paused == paused && paused_set == paused) {
        return;
    }

    Pause(is_paused);
    if (timer_thread.joinable()) {
        if (!is_paused) {
            pause_event.Set();
        }
        event.Set();
        while (paused_set != is_paused)
            ;
    }

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

bool CoreTiming::IsRunning() const {
    return !paused_set;
}

bool CoreTiming::HasPendingEvents() const {
    std::scoped_lock lock{basic_lock};
    return !(wait_set && event_queue.empty());
}

void CoreTiming::ScheduleEvent(std::chrono::nanoseconds ns_into_future,
                               const std::shared_ptr<EventType>& event_type, bool absolute_time) {
    // Wake the timing thread only when the new event is due before the one it waits for: every
    // schedule used to wake it (~270k wakes/s against ~2.6k fired events/s in MHGU).
    bool wake;
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? ns_into_future : GetGlobalTimeNs() + ns_into_future};

        wake = event_queue.empty() || next_time.count() < event_queue.top().time;
        auto h{event_queue.emplace(Event{next_time.count(), event_fifo_id++, event_type, 0})};
        (*h).handle = h;
    }

    if (wake) {
        event.Set();
    }
}

void CoreTiming::ScheduleLoopingEvent(std::chrono::nanoseconds start_time,
                                      std::chrono::nanoseconds resched_time,
                                      const std::shared_ptr<EventType>& event_type,
                                      bool absolute_time) {
    bool wake;
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? start_time : GetGlobalTimeNs() + start_time};

        wake = event_queue.empty() || next_time.count() < event_queue.top().time;
        auto h{event_queue.emplace(
            Event{next_time.count(), event_fifo_id++, event_type, resched_time.count()})};
        (*h).handle = h;
    }

    if (wake) {
        event.Set();
    }
}

void CoreTiming::UnscheduleEvent(const std::shared_ptr<EventType>& event_type,
                                 UnscheduleEventType type) {
    {
        std::scoped_lock lk{basic_lock};

        std::vector<heap_t::handle_type> to_remove;
        for (auto itr = event_queue.begin(); itr != event_queue.end(); itr++) {
            const Event& e = *itr;
            if (e.type.lock().get() == event_type.get()) {
                to_remove.push_back(itr->handle);
            }
        }

        for (auto& h : to_remove) {
            event_queue.erase(h);
        }

        event_type->sequence_number++;
    }

    // Force any in-progress events to finish
    if (type == UnscheduleEventType::Wait) {
        std::scoped_lock lk{advance_lock};
    }
}

static u64 GetNextTickCount(u64 next_ticks) {
    if (Settings::values.use_custom_cpu_ticks.GetValue()) {
        return Settings::values.cpu_ticks.GetValue();
    }
    return next_ticks;
}

void CoreTiming::AddTicks(u64 ticks_to_add) {
    const u64 ticks = GetNextTickCount(ticks_to_add);
    cpu_ticks += ticks;
    downcount -= static_cast<s64>(ticks);
}

void CoreTiming::Idle() {
    AddTicks(1000U);
}

void CoreTiming::ResetTicks() {
    downcount = MAX_SLICE_LENGTH;
}

u64 CoreTiming::GetClockTicks() const {
    u64 fres = is_multicore ? Common::g_wall_clock.GetCNTPCT() : Common::WallClock::CPUTickToCNTPCT(cpu_ticks);
    if (auto const overclock = Settings::values.fast_cpu_time.GetValue(); overclock != Settings::CpuClock::Off) {
        fres = u64(f64(fres) * (1.7 + 0.3 * u32(overclock)));
    }
    if (::Settings::values.sync_core_speed.GetValue()) {
        auto const ticks = f64(fres);
        auto const speed_limit = f64(Settings::SpeedLimit()) * 0.01;
        return u64(ticks / speed_limit);
    }
    return fres;
}

u64 CoreTiming::GetGPUTicks() const {
    return is_multicore
        ? Common::g_wall_clock.GetGPUTick()
        : Common::WallClock::CPUTickToGPUTick(cpu_ticks);
}

std::optional<s64> CoreTiming::Advance() {
    std::scoped_lock lock{advance_lock, basic_lock};
    global_timer = GetGlobalTimeNs().count();

    while (!event_queue.empty() && event_queue.top().time <= global_timer) {
        const Event& evt = event_queue.top();

        if (const auto event_type{evt.type.lock()}) {
            if (t_stats) {
                t_stats->Fired(event_type.get());
            }
            const auto evt_time = evt.time;
            const auto evt_sequence_num = event_type->sequence_number;

            if (evt.reschedule_time == 0) {
                event_queue.pop();

                basic_lock.unlock();

                event_type->callback(
                    evt_time, std::chrono::nanoseconds{GetGlobalTimeNs().count() - evt_time});

                basic_lock.lock();
            } else {
                basic_lock.unlock();

                const auto new_schedule_time{event_type->callback(
                    evt_time, std::chrono::nanoseconds{GetGlobalTimeNs().count() - evt_time})};

                basic_lock.lock();

                if (evt_sequence_num != event_type->sequence_number) {
                    // Heap handle is invalidated after external modification.
                    continue;
                }

                const auto next_schedule_time{new_schedule_time.has_value()
                                                  ? new_schedule_time.value().count()
                                                  : evt.reschedule_time};

                // If this event was scheduled into a pause, its time now is going to be way
                // behind. Re-set this event to continue from the end of the pause.
                auto next_time{evt.time + next_schedule_time};
                if (evt.time < pause_end_time) {
                    next_time = pause_end_time + next_schedule_time;
                }

                event_queue.update(evt.handle, Event{next_time, event_fifo_id++, evt.type,
                                                     next_schedule_time, evt.handle});
            }
        }

        global_timer = GetGlobalTimeNs().count();
    }

    if (!event_queue.empty()) {
        return event_queue.top().time;
    } else {
        return std::nullopt;
    }
}

void CoreTiming::Reset() {
    paused = true;
    pause_event.Set();
    event.Set();
    if (timer_thread.joinable()) {
        timer_thread.request_stop();
        timer_thread.join();
    }
    has_started = false;
}

/// @brief Returns current time in nanoseconds.
std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const noexcept {
    return is_multicore
        ? Common::g_wall_clock.GetTimeNS()
        : std::chrono::nanoseconds{Common::WallClock::CPUTickToNS(cpu_ticks)};
}

/// @brief Returns current time in microseconds.
std::chrono::microseconds CoreTiming::GetGlobalTimeUs() const noexcept {
    return is_multicore
        ? Common::g_wall_clock.GetTimeUS()
        : std::chrono::microseconds{Common::WallClock::CPUTickToUS(cpu_ticks)};
}

#ifdef _WIN32
void CoreTiming::SetTimerResolutionNs(std::chrono::nanoseconds ns) {
    timer_resolution_ns = ns.count();
}
#endif

} // namespace Core::Timing
