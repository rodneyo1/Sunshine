// Standalone Windows single-owner receiver/waiter reference; no sockets or OS
// injection. Caller must join run() before destroying this object or backend.
#pragma once
#include "receiver.h"
#include "windows-keys.h"
#include "windows-wait.h"
#include "windows-clock.h"
#include <atomic>
#include <mutex>

namespace steady {
enum class Submit { accepted, rejected, full, closed };
struct OwnerCounts {
    u64 submitted = 0, rejected = 0, processed = 0, queue_abandoned = 0;
    u64 queue_overflows = 0, faults = 0, waits = 0;
    ClockFailure clock_failure = ClockFailure::none;
};
template<std::size_t Q, std::size_t N, std::size_t W, class Backend, class ClockSource = WindowsSessionClock> class WindowsOwner {
    static_assert(Q > 0 && Q <= 4096, "Bounded ingress queue");
    // Record initializes every member; avoid expanding a large aggregate
    // value initializer in MSVC, as with the bounded sender history.
    std::array<Record,Q> queue_;
    std::size_t head_ = 0, used_ = 0;
    std::mutex mutex_;
    bool closing_ = false, ingress_fault_ = false;
    u64 generation_;
    WindowsWait wait_;
    ClockSource clock_;
    ClockValidity validity_ {ClockAgreement{}};
    bool guarded_ = false;
    Receiver<N,W,Backend> receiver_;
    OwnerCounts counts_ {};
    std::atomic<bool> started_ {false};
    std::atomic<u64> processed_ {0}, waits_ {0};
    bool check_clock_locked() noexcept {
        if (!guarded_) return true;
        ClockSample sample;
        if (!clock_.sample(sample)) validity_.close(ClockFailure::unavailable);
        else validity_.observe(sample);
        if (validity_.valid()) return true;
        counts_.clock_failure = validity_.failure();
        closing_ = true; ingress_fault_ = true;
        return false;
    }
    bool check_clock() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return check_clock_locked();
    }
    static bool dispatch_check(void* context) noexcept {
        return static_cast<WindowsOwner*>(context)->check_clock();
    }
public:
    WindowsOwner(u64 generation, FrozenMapping mapping, Backend& backend)
        : generation_(generation), receiver_(generation,mapping,backend) {}
    // This overload enforces a previously admitted local clock agreement. It
    // does not manufacture calibration quality or authenticate peer metadata.
    WindowsOwner(u64 generation, FrozenMapping mapping, Backend& backend,
                 ClockAgreement agreement, ClockSource source = {})
        : generation_(generation), clock_(source), validity_(agreement), guarded_(true),
          receiver_(generation,mapping,backend,{dispatch_check,this}) {
        if (generation != agreement.generation) validity_.close(ClockFailure::invalid_agreement);
    }
    WindowsOwner(const WindowsOwner&) = delete;
    WindowsOwner& operator=(const WindowsOwner&) = delete;
    bool ready() const noexcept { return generation_ && clock_.ready() && wait_.ready(); }
    u64 processed() const noexcept { return processed_.load(); }
    u64 waits() const noexcept { return waits_.load(); }
    // Authentication, ownership and the complete profile precede queue mutation.
    Submit submit(const Record& record, bool authenticated) {
        Submit result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.submitted;
            if (closing_) return Submit::closed;
            if (!authenticated || record.generation != generation_ || !windows_record(record)) {
                ++counts_.rejected; return Submit::rejected;
            }
            if (!check_clock_locked()) { wait_.cancel(); return Submit::closed; }
            if (used_ == Q) {
                ++counts_.queue_overflows; closing_ = true; ingress_fault_ = true; result = Submit::full;
            } else {
                queue_[(head_+used_)%Q] = record; ++used_; result = Submit::accepted;
            }
        }
        if (result == Submit::full) wait_.cancel();
        else if (!wait_.changed()) {
            { std::lock_guard<std::mutex> lock(mutex_); closing_ = true; ingress_fault_ = true; }
            wait_.cancel(); return Submit::closed;
        }
        return result;
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); closing_ = true;
          if (guarded_ && counts_.clock_failure == ClockFailure::none)
              counts_.clock_failure = ClockFailure::disconnected; }
        wait_.cancel();
    }
    bool peer_clock_invalidated(bool authenticated, u64 generation) {
        bool closed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (guarded_ && validity_.peer_invalidated(authenticated,generation)) {
                counts_.clock_failure = validity_.failure();
                closing_ = true; ingress_fault_ = true; closed = true;
            }
        }
        if (closed) wait_.cancel();
        return closed;
    }
    bool run() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected,true)) return false;
        bool good = ready();
        while (good) {
            if (!check_clock()) { good = false; break; }
            bool closing = false;
            for (unsigned batch = 0; batch < 64; ++batch) {
                Record record; bool have = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    closing = closing_;
                    if (!closing && used_) {
                        record = queue_[head_]; head_ = (head_+1)%Q; --used_; have = true;
                    }
                }
                if (closing || !have) break;
                u64 now;
                if (!check_clock() || !clock_.now(now)) {
                    // This record already left ingress but never reached the
                    // receiver. Retain it in the original-event inventory.
                    { std::lock_guard<std::mutex> lock(mutex_); ++counts_.queue_abandoned; }
                    good = false; break;
                }
                receiver_.receive(record,true,now);
                processed_.fetch_add(1);
                if (receiver_.closed()) { good = false; break; }
            }
            if (closing || !good) break;
            u64 now, deadline, ticks;
            if (!check_clock() || !clock_.now(now)) { good = false; break; }
            receiver_.advance(now);
            if (receiver_.closed()) { good = false; break; }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closing_) break;
                if (used_) continue; // Bounded batches still service due deadlines.
            }
            if (!receiver_.next_deadline(deadline)) {
                if (now > std::numeric_limits<u64>::max()-1000000000) { good = false; break; }
                deadline = now+1000000000;
            }
            if (guarded_) {
                if (now > std::numeric_limits<u64>::max()-10000000) { good = false; break; }
                deadline = std::min(deadline,std::min(validity_.expires(),now+10000000));
            }
            if (!clock_.deadline_ticks(deadline,ticks)) { good = false; break; }
            waits_.fetch_add(1);
            const auto wake = wait_.until(ticks);
            if (wake == Wake::cancelled) break;
            if (wake == Wake::fault) { good = false; break; }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
            if (ingress_fault_) good = false;
            counts_.queue_abandoned += used_; used_ = 0;
            counts_.processed = processed_.load(); counts_.waits = waits_.load();
            if (!good) ++counts_.faults;
        }
        receiver_.disconnect(); // Always on this owner, including backend cleanup.
        return good;
    }
    // Read only after run() has returned and its thread has joined.
    const OwnerCounts& counts_after_join() const noexcept { return counts_; }
    const ReceiverCounts& receiver_counts_after_join() const noexcept { return receiver_.counts(); }
};
} // namespace steady
