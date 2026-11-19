// Process-lifetime Windows continuity source, independent of forwarded keys.
#pragma once
#include "clock-agreement.h"
#include "windows-wait.h"
#include <powrprof.h>
#include <bcrypt.h>
#include <atomic>

namespace steady {
class QpcNanoseconds {
    u64 frequency_ = 0;
public:
    QpcNanoseconds() noexcept {
        LARGE_INTEGER f {};
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0 &&
            static_cast<u64>(f.QuadPart) <= std::numeric_limits<u64>::max()/1000000000)
            frequency_ = static_cast<u64>(f.QuadPart);
    }
    bool ready() const noexcept { return frequency_ != 0; }
    u64 frequency() const noexcept { return frequency_; }
    bool now(u64& ns) const noexcept {
        LARGE_INTEGER t {};
        if (!ready() || !QueryPerformanceCounter(&t) || t.QuadPart < 0) return false;
        const auto ticks = static_cast<u64>(t.QuadPart), whole = ticks/frequency_;
        const auto fraction = (ticks%frequency_)*1000000000/frequency_;
        if (whole > (std::numeric_limits<u64>::max()-fraction)/1000000000) return false;
        ns = whole*1000000000+fraction; return true;
    }
    bool deadline_ticks(u64 ns, u64& ticks) const noexcept {
        if (!ready()) return false;
        const auto whole = ns/1000000000, product = (ns%1000000000)*frequency_;
        const auto fraction = product/1000000000 + (product%1000000000 != 0);
        if (whole > (std::numeric_limits<u64>::max()-fraction)/frequency_) return false;
        ticks = whole*frequency_+fraction; return true;
    }
};

class PowerEpochCounter {
    std::atomic<u64> epoch_ {1};
    std::atomic<bool> fault_ {false};
public:
    explicit PowerEpochCounter(u64 initial = 1) noexcept : epoch_(initial), fault_(!initial) {}
    void notify(ULONG type) noexcept {
        if (type != PBT_APMSUSPEND && type != PBT_APMRESUMESUSPEND && type != PBT_APMRESUMEAUTOMATIC) return;
        auto old = epoch_.load();
        for (;;) {
            if (old == std::numeric_limits<u64>::max()) { fault_.store(true); return; }
            if (epoch_.compare_exchange_weak(old,old+1)) return;
        }
    }
    bool value(u64& epoch) const noexcept {
        if (fault_.load()) return false;
        epoch = epoch_.load(); return true;
    }
};
class WindowsPowerEpoch {
    u64 incarnation_[2] {};
    PowerEpochCounter epoch_;
    HPOWERNOTIFY registration_ = nullptr;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters_ {};
    static ULONG CALLBACK callback(PVOID context, ULONG type, PVOID) noexcept {
        auto& state = *static_cast<WindowsPowerEpoch*>(context);
        state.epoch_.notify(type);
        return ERROR_SUCCESS;
    }
    WindowsPowerEpoch() noexcept {
        if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(incarnation_),sizeof(incarnation_),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !(incarnation_[0] || incarnation_[1])) return;
        parameters_ = {callback,this};
        registration_ = RegisterSuspendResumeNotification(&parameters_,DEVICE_NOTIFY_CALLBACK);
    }
public:
    WindowsPowerEpoch(const WindowsPowerEpoch&) = delete;
    WindowsPowerEpoch& operator=(const WindowsPowerEpoch&) = delete;
    // One fixed allocation/registration per process; never per connection.
    // Its callback context remains alive until process exit, avoiding teardown
    // races with system callbacks. Windows reclaims its registration at exit.
    static WindowsPowerEpoch& process() {
        static auto* state = new WindowsPowerEpoch;
        return *state;
    }
    bool identity(u64 frequency, ClockIdentity& out) const noexcept {
        u64 epoch = 0;
        if (!registration_ || !epoch_.value(epoch) || !frequency) return false;
        out = {incarnation_[0],incarnation_[1],epoch,frequency};
        return true;
    }
};
class WindowsSessionClock : public QpcNanoseconds {
    WindowsPowerEpoch& power_ = WindowsPowerEpoch::process();
public:
    bool read_frequency(u64& frequency) const noexcept {
        LARGE_INTEGER f {};
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return false;
        frequency = static_cast<u64>(f.QuadPart); return true;
    }
    bool read_identity(u64 frequency, ClockIdentity& identity) const noexcept {
        return power_.identity(frequency,identity);
    }
    bool sample(ClockSample& out) const noexcept {
        return consistent_clock_sample(*this,frequency(),out);
    }
};
} // namespace steady
