// Single-owner offline receiver. Backend supplies bool send(Key,bool) noexcept:
// false means delivery is uncertain, so teardown must attempt release as well.
#pragma once
#include "core.h"
#include <utility>

namespace steady {
enum class Receive { queued, late, ignored, rejected, fault, closed };
struct ReceiverCounts {
    u64 arrivals = 0, admitted = 0, ignored = 0, rejected = 0;
    u64 dispatched = 0, late = 0, transitions = 0, repairs = 0;
    u64 superseded = 0, redundant = 0, faults = 0, abandoned = 0;
    u64 cleanup_attempts = 0, cleanup_failures = 0;
    u64 overdue_dispatches = 0, max_dispatch_lateness = 0;
};
struct DispatchGuard {
    bool (*check)(void*) noexcept = nullptr;
    void* context = nullptr;
    bool allowed() const noexcept { return !check || check(context); }
};

// N bounds pending records; W bounds replay and source-order history. This
// reference scans fixed storage, and is not a performance-qualified scheduler.
template<std::size_t N, std::size_t W, class Backend> class Receiver {
    static_assert(N > 0 && W > 0, "Nonempty receiver storage required");
    static_assert(noexcept(std::declval<Backend&>().send(Key{},false)), "Backend must not throw");
    struct Pending { Record record {}; u64 deadline = 0; };
    struct Stamp { u64 seq = 0, capture = 0; bool valid = false; };
    // Both element types initialize all members. Default initialization avoids
    // large aggregate expansion without changing record or validity defaults.
    std::array<Pending, N> pending_;
    std::array<Stamp, W> stamps_;
    std::array<bool, 256> possibly_down_ {};
    std::size_t used_ = 0;
    SeenWindow<W> seen_;
    FrozenMapping mapping_;
    KeyLedger ledger_;
    Backend& backend_;
    u64 generation_, last_now_ = 0;
    u64 dispatched_capture_ = 0, dispatched_seq_ = 0;
    bool dispatched_any_ = false;
    u64 source_high_ = 0, source_capture_ = 0;
    bool source_started_ = false;
    bool clock_started_ = false, closed_ = false;
    ReceiverCounts counts_ {};
    DispatchGuard guard_;

    void teardown(bool fault) {
        if (closed_) return;
        closed_ = true;
        counts_.faults += fault ? 1 : 0;
        counts_.abandoned += used_;
        used_ = 0;
        for (u16 i = 4; i < 256; ++i) {
            if (!possibly_down_[i]) continue;
            ++counts_.cleanup_attempts;
            if (!backend_.send({7,i}, false)) ++counts_.cleanup_failures;
            possibly_down_[i] = false;
        }
        ledger_.disconnect();
    }
    bool clock(u64 now) {
        if (clock_started_ && now < last_now_) { teardown(true); return false; }
        clock_started_ = true; last_now_ = now; return true;
    }
    bool supported(const Record& r) const {
        if (r.kind == Kind::key) return r.key.usage >= 4 && r.key.usage < 256;
        if (r.kind == Kind::state) {
            for (std::size_t i = 0; i < r.count; ++i)
                if (r.held[i].usage < 4 || r.held[i].usage >= 256) return false;
        }
        return true;
    }
    bool emit(Key key, bool down) {
        if (!guard_.allowed()) { teardown(true); return false; }
        // A failed keydown may have reached the OS. Retain ownership until
        // teardown tries keyup. Failed keyup also retains possible ownership.
        if (down) possibly_down_[key.usage] = true;
        if (!backend_.send(key,down)) { teardown(true); return false; }
        possibly_down_[key.usage] = down;
        ++counts_.transitions;
        return true;
    }
    void dispatch(const Record& r, u64 deadline, u64 now) {
        if (!guard_.allowed()) { ++counts_.abandoned; teardown(true); return; }
        ++counts_.dispatched;
        if (now > deadline) {
            ++counts_.overdue_dispatches;
            counts_.max_dispatch_lateness = std::max(counts_.max_dispatch_lateness,now-deadline);
        }
        if (!dispatched_any_ || r.capture > dispatched_capture_ ||
            (r.capture == dispatched_capture_ && r.seq > dispatched_seq_)) {
            dispatched_capture_ = r.capture; dispatched_seq_ = r.seq; dispatched_any_ = true;
        }
        // Commit logical state only after all backend operations succeed.
        auto next = ledger_;
        if (r.kind == Kind::key) {
            const auto result = next.apply(r.key,r.down,r.seq);
            if (result == Apply::superseded) ++counts_.superseded;
            else if (result == Apply::redundant) ++counts_.redundant;
            else if (result == Apply::transition && !emit(r.key,r.down)) return;
        } else if (r.kind == Kind::state) {
            const int repairs = next.reconcile(r);
            if (repairs < 0) { ++counts_.superseded; return; }
            // Releases precede presses, including modifier state repairs.
            for (bool down : {false,true}) {
                for (u16 i = 4; i < 256; ++i) {
                    if (ledger_.held(i) != next.held(i) && next.held(i) == down) {
                        if (!emit({7,i},down)) return;
                        ++counts_.repairs;
                    }
                }
            }
        }
        ledger_ = next;
    }
public:
    Receiver(u64 generation, FrozenMapping mapping, Backend& backend, DispatchGuard guard = {}) :
        seen_(generation), mapping_(mapping), backend_(backend), generation_(generation), guard_(guard) {}
    ~Receiver() { teardown(false); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    // Backend must outlive this object; explicit teardown permits reporting
    // cleanup errors while the session is still observable. Destructor is safe.
    void disconnect() { teardown(false); }
    bool closed() const { return closed_; }
    std::size_t pending() const { return used_; }
    const ReceiverCounts& counts() const { return counts_; }
    bool next_deadline(u64& out) const {
        if (!used_) return false;
        out = pending_[0].deadline;
        for (std::size_t i = 1; i < used_; ++i) out = std::min(out,pending_[i].deadline);
        return true;
    }
    void advance(u64 now) {
        if (closed_ || !clock(now)) return;
        while (used_ && !closed_) {
            std::size_t first = 0;
            for (std::size_t i = 1; i < used_; ++i) {
                const auto& a = pending_[i].record;
                const auto& b = pending_[first].record;
                if (a.capture < b.capture || (a.capture == b.capture && a.seq < b.seq)) first = i;
            }
            if (pending_[first].deadline > now) break;
            const auto r = pending_[first].record;
            const auto deadline = pending_[first].deadline;
            pending_[first] = pending_[--used_];
            dispatch(r,deadline,now);
        }
    }
    Receive receive(const Record& r, bool authenticated, u64 now) {
        if (closed_) return Receive::closed;
        ++counts_.arrivals;
        // Invalid/unowned traffic cannot advance time, dedup or source state.
        if (!authenticated || r.generation != generation_ || validate(r) != Parse::ok || !supported(r)) {
            ++counts_.rejected; return Receive::rejected;
        }
        advance(now);
        if (closed_) return Receive::fault;
        const auto admission = seen_.accept(true,r.generation,r.seq);
        if (admission != Admission::accepted) { ++counts_.ignored; return Receive::ignored; }
        ++counts_.admitted;
        if (source_started_ && r.seq > source_high_ && r.capture < source_capture_) {
            ++counts_.abandoned; teardown(true); return Receive::fault;
        }
        // All original records, including STATE/PROBE, share monotonically
        // nondecreasing source timestamps. History expires with replay window.
        for (const auto& s : stamps_) {
            if (!s.valid) continue;
            const u64 distance = s.seq > r.seq ? s.seq-r.seq : r.seq-s.seq;
            if (distance >= W) continue;
            if ((s.seq < r.seq && s.capture > r.capture) || (s.seq > r.seq && s.capture < r.capture)) {
                ++counts_.abandoned; teardown(true); return Receive::fault;
            }
        }
        // Pending records remain relevant even beyond replay history.
        for (std::size_t i = 0; i < used_; ++i) {
            const auto& s = pending_[i].record;
            if ((s.seq < r.seq && s.capture > r.capture) || (s.seq > r.seq && s.capture < r.capture)) {
                ++counts_.abandoned; teardown(true); return Receive::fault;
            }
        }
        stamps_[r.seq % W] = {r.seq,r.capture,true};
        if (!source_started_ || r.seq > source_high_) {
            source_high_ = r.seq; source_capture_ = r.capture; source_started_ = true;
        }
        u64 deadline = 0;
        if (!mapping_.deadline(r.capture,deadline)) {
            ++counts_.abandoned; teardown(true); return Receive::fault;
        }
        const bool historical = dispatched_any_ && (r.capture < dispatched_capture_ ||
            (r.capture == dispatched_capture_ && r.seq < dispatched_seq_));
        if (deadline < now || historical) {
            ++counts_.late; dispatch(r,deadline,now); return closed_ ? Receive::fault : Receive::late;
        }
        if (used_ == N) { ++counts_.abandoned; teardown(true); return Receive::fault; }
        pending_[used_++] = {r,deadline};
        advance(now); // Exact-deadline arrivals are on time and immediately due.
        return closed_ ? Receive::fault : Receive::queued;
    }
};
} // namespace steady
