// Frozen-agreement lifecycle only. Admission still requires calibrated bounds,
// authenticated peer identities, profile and ownership validation by the caller.
#pragma once
#include "core.h"

namespace steady {
struct ClockIdentity {
    u64 incarnation_high = 0, incarnation_low = 0, epoch = 0, frequency = 0;
    bool valid() const noexcept {
        return (incarnation_high || incarnation_low) && epoch && frequency;
    }
};
inline bool operator==(ClockIdentity a, ClockIdentity b) noexcept {
    return a.incarnation_high == b.incarnation_high && a.incarnation_low == b.incarnation_low &&
        a.epoch == b.epoch && a.frequency == b.frequency;
}
struct ClockSample { u64 now = 0; ClockIdentity identity {}; };
// Bracket time with independent identity/frequency reads. A transition during
// sampling must not seed a new agreement using a pre-transition timestamp.
template<class Source> bool consistent_clock_sample(Source& source, u64 expected_frequency, ClockSample& output) noexcept {
    ClockSample sample; ClockIdentity after; u64 before_frequency = 0, after_frequency = 0;
    if (!expected_frequency || !source.read_frequency(before_frequency) || before_frequency != expected_frequency ||
        !source.read_identity(before_frequency,sample.identity) || !sample.identity.valid() ||
        sample.identity.frequency != before_frequency || !source.now(sample.now) ||
        !source.read_frequency(after_frequency) || after_frequency != before_frequency ||
        !source.read_identity(after_frequency,after) || !(sample.identity == after)) return false;
    output = sample; return true;
}
struct ClockAgreement {
    u64 generation = 0, calibrated_through = 0, valid_until = 0;
    ClockIdentity local {};
};
// Outward-rounded bounds from complete independent calibration. The rate
// bound is a conservative growth bound covering BOTH source-pair duration and
// admitted local-clock age, in parts per billion, not an observed RTT. If the
// domains have different rates, the estimator must round their worst bound up.
struct ClockErrorBounds {
    u64 initial_error_ns = 0, rate_error_ppb = 0;
    bool available = false;
};
struct ClockBudget {
    u64 absolute_error_ns = 0, pair_error_ns = 0, maximum_age_ns = 0;
};
inline bool clock_error_growth(u64 duration, u64 rate_error_ppb, u64& error) noexcept {
    if (!rate_error_ppb) { error = 0; return true; }
    u64 floor = 0, remainder = 0;
    if (!scale_ratio_floor(duration,rate_error_ppb,1000000000,floor,remainder) ||
        (remainder && floor == std::numeric_limits<u64>::max())) return false;
    error = floor + (remainder != 0); return true;
}
// Derive a finite local expiry from explicit budgets; no arbitrary long-session
// lifetime. Bounds remain conditional on the calibration's clock model and
// continuity. This does not authenticate or independently validate calibration.
inline bool make_clock_agreement(u64 generation, ClockIdentity identity, u64 calibrated_through,
                                 ClockErrorBounds bounds, ClockBudget budget, ClockAgreement& output) noexcept {
    if (!generation || !identity.valid() || !bounds.available || !budget.maximum_age_ns ||
        bounds.initial_error_ns > budget.absolute_error_ns) return false;
    u64 pair_error = 0;
    if (!clock_error_growth(2000000000,bounds.rate_error_ppb,pair_error) || pair_error > budget.pair_error_ns)
        return false;
    const auto allowance = budget.absolute_error_ns-bounds.initial_error_ns;
    u64 age = budget.maximum_age_ns, growth = 0;
    if (!clock_error_growth(age,bounds.rate_error_ppb,growth) || growth > allowance) {
        u64 remainder = 0;
        if (!bounds.rate_error_ppb || !scale_ratio_floor(allowance,1000000000,bounds.rate_error_ppb,age,remainder))
            return false;
    }
    if (!age || age > std::numeric_limits<u64>::max()-calibrated_through) return false;
    output = {generation,calibrated_through,calibrated_through+age,identity};
    return true;
}
enum class ClockFailure {
    none, invalid_agreement, unavailable, identity_changed, clock_reversed,
    expired, peer_changed, disconnected
};
class ClockValidity {
    ClockAgreement agreement_;
    ClockFailure failure_ = ClockFailure::none;
    u64 last_;
public:
    explicit ClockValidity(ClockAgreement agreement) noexcept
        : agreement_(agreement), last_(agreement.calibrated_through) {
        if (!agreement.generation || !agreement.local.valid() || agreement.valid_until <= agreement.calibrated_through)
            failure_ = ClockFailure::invalid_agreement;
    }
    ClockFailure failure() const noexcept { return failure_; }
    bool valid() const noexcept { return failure_ == ClockFailure::none; }
    u64 expires() const noexcept { return agreement_.valid_until; }
    u64 generation() const noexcept { return agreement_.generation; }
    void close(ClockFailure reason) noexcept {
        if (valid() && reason != ClockFailure::none) failure_ = reason;
    }
    bool observe(const ClockSample& sample) noexcept {
        if (!valid()) return false;
        if (!(sample.identity == agreement_.local)) close(ClockFailure::identity_changed);
        else if (sample.now < last_) close(ClockFailure::clock_reversed);
        else if (sample.now >= agreement_.valid_until) close(ClockFailure::expired);
        else last_ = sample.now;
        return valid();
    }
    // Call only with authenticated transport metadata. Old generations and
    // unauthenticated traffic cannot cancel or revive the current agreement.
    bool peer_invalidated(bool authenticated, u64 generation) noexcept {
        if (!authenticated || generation != agreement_.generation || !valid()) return false;
        close(ClockFailure::peer_changed); return true;
    }
};
} // namespace steady
