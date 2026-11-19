// Windows wait/cancellation reference; no input injection or priority changes.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <limits>

namespace steady {
enum class Wake { due, changed, cancelled, fault };
class WindowsWait {
    HANDLE timer_ = nullptr, changed_ = nullptr, cancel_ = nullptr;
    std::uint64_t frequency_ = 0;
    bool high_resolution_ = false;
public:
    WindowsWait() {
        LARGE_INTEGER frequency {};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            frequency.QuadPart > std::numeric_limits<LONGLONG>::max()/10000000) return;
        frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
        timer_ = CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        high_resolution_ = timer_ != nullptr;
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
        changed_ = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        cancel_ = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    }
    ~WindowsWait() {
        if (timer_) CloseHandle(timer_);
        if (changed_) CloseHandle(changed_);
        if (cancel_) CloseHandle(cancel_);
    }
    WindowsWait(const WindowsWait&) = delete;
    WindowsWait& operator=(const WindowsWait&) = delete;
    bool ready() const { return frequency_ && timer_ && changed_ && cancel_; }
    bool high_resolution() const { return high_resolution_; }
    std::uint64_t frequency() const { return frequency_; }
    bool now(std::uint64_t& out) const {
        LARGE_INTEGER stamp {};
        if (!QueryPerformanceCounter(&stamp) || stamp.QuadPart < 0) return false;
        out = static_cast<std::uint64_t>(stamp.QuadPart); return true;
    }
    bool changed() { return changed_ && SetEvent(changed_); }
    bool cancel() { return cancel_ && SetEvent(cancel_); }
    // One owner waits; producer may signal changed/cancel while handles live.
    // On changed, owner reads its synchronized queue and computes a new head.
    Wake until(std::uint64_t deadline) {
        if (!ready()) return Wake::fault;
        const HANDLE handles[] = {cancel_,changed_,timer_};
        for (;;) {
            if (WaitForSingleObject(cancel_,0) == WAIT_OBJECT_0) return Wake::cancelled;
            if (WaitForSingleObject(changed_,0) == WAIT_OBJECT_0) return Wake::changed;
            std::uint64_t stamp;
            if (!now(stamp)) return Wake::fault;
            if (stamp >= deadline) return Wake::due;
            const auto remaining = deadline-stamp;
            const auto whole = remaining/frequency_;
            const auto fraction = remaining%frequency_;
            const auto max = static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
            if (whole > (max-10000000)/10000000) return Wake::fault;
            // Round up to 100 ns; recheck QPC after every timer wake. No
            // application spin, timeBeginPeriod, affinity or priority changes.
            const auto units = whole*10000000 + (fraction*10000000+frequency_-1)/frequency_;
            LARGE_INTEGER due {}; due.QuadPart = -static_cast<LONGLONG>(units ? units : 1);
            if (!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)) return Wake::fault;
            const auto result = WaitForMultipleObjects(3,handles,FALSE,INFINITE);
            CancelWaitableTimer(timer_);
            if (result == WAIT_OBJECT_0) return Wake::cancelled;
            if (result == WAIT_OBJECT_0+1) return Wake::changed;
            if (result != WAIT_OBJECT_0+2) return Wake::fault;
        }
    }
};
} // namespace steady
