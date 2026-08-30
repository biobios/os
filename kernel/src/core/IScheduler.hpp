#pragma once
#include <concepts>
#include <cstdint>
#include "core/Thread.hpp"

namespace oz {

template <typename Scheduler>
concept scheduler = requires(Scheduler sched, std::uint8_t level, void (*entry)(void*), void* arg, Thread* thread) {
    { sched.createThread(level, entry, arg) } -> std::same_as<Thread*>;
    { sched.initMainThread() } -> std::same_as<void>;
    { sched.queueThread(thread) } -> std::same_as<void>;
    { sched.schedule() } -> std::same_as<void>;
    { sched.tick() } -> std::same_as<void>;
    { sched.sleep(std::uint64_t(0)) } -> std::same_as<void>;
    { sched.getTicks() } -> std::same_as<std::uint64_t>;
    { sched.exitThread() } -> std::same_as<void>;
    { sched.getCurrentThread() } -> std::same_as<Thread*>;
    { sched.reapZombies() } -> std::same_as<void>;
};

template <typename Accessor>
concept scheduler_accessor = requires() {
    typename Accessor::Settings::Scheduler;
    { Accessor::getScheduler() } -> std::same_as<typename Accessor::Settings::Scheduler&>;
} && scheduler<typename Accessor::Settings::Scheduler>;

} // namespace oz
