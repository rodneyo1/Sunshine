// Offline native reference primitives. Logical type IDs are harness-local,
// not upstream packet/channel allocations. No sockets, threads or OS input.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace steady {
using u16 = std::uint16_t;
using u64 = std::uint64_t;
constexpr std::size_t header_bytes = 32;
constexpr std::size_t max_held = 256;
constexpr std::size_t max_record = header_bytes + 12 + max_held * 4;
enum class Kind : u16 { key = 1, state = 2, probe = 3 };
struct Key { u16 page = 7, usage = 0; };
inline bool operator==(Key a, Key b) { return a.page == b.page && a.usage == b.usage; }
inline bool operator<(Key a, Key b) { return a.page < b.page || (a.page == b.page && a.usage < b.usage); }
struct Record {
    u64 generation = 0, seq = 0, capture = 0;
    Kind kind = Kind::probe;
    Key key {};
    bool down = false;
    u64 through = 0;
    u16 count = 0;
    std::array<Key, max_held> held {};
};
enum class Parse { ok, length, version, flags, type, payload, watermark, state_order };
inline u64 read_be(const std::uint8_t* p, unsigned n) {
    u64 value = 0;
    for (unsigned i = 0; i < n; ++i) value = (value << 8) | p[i];
    return value;
}
inline void write_be(std::uint8_t* p, unsigned n, u64 value) {
    for (unsigned i = n; i > 0; --i) { p[i - 1] = static_cast<std::uint8_t>(value); value >>= 8; }
}
inline Parse validate(const Record& r) {
    if (r.kind == Kind::key) return r.key.page == 7 ? Parse::ok : Parse::payload;
    if (r.kind == Kind::probe) return Parse::ok;
    if (r.kind != Kind::state) return Parse::type;
    if (r.count > max_held) return Parse::length;
    if (r.through >= r.seq) return Parse::watermark;
    for (std::size_t i = 0; i < r.count; ++i) {
        if (r.held[i].page != 7) return Parse::payload;
        if (i && !(r.held[i - 1] < r.held[i])) return Parse::state_order;
    }
    return Parse::ok;
}
// Output is committed only after complete validation. Authentication and the
// negotiated outer-envelope size budget belong to the caller.
inline Parse decode(const std::uint8_t* data, std::size_t size, std::size_t budget, Record& output) {
    if (!data || size < header_bytes || size > budget || size > max_record) return Parse::length;
    if (data[0] != 1) return Parse::version;
    if (data[1]) return Parse::flags;
    if (read_be(data + 2, 2) != header_bytes || read_be(data + 30, 2) != size - header_bytes) return Parse::length;
    Record r;
    r.generation = read_be(data + 4, 8); r.seq = read_be(data + 12, 8); r.capture = read_be(data + 20, 8);
    r.kind = static_cast<Kind>(read_be(data + 28, 2));
    const auto* body = data + header_bytes;
    if (r.kind == Kind::key) {
        if (size != header_bytes + 8) return Parse::length;
        if (body[4] > 1 || body[5] || read_be(body + 6, 2)) return Parse::payload;
        r.key = {static_cast<u16>(read_be(body, 2)), static_cast<u16>(read_be(body + 2, 2))};
        r.down = body[4] != 0;
    } else if (r.kind == Kind::state) {
        if (size < header_bytes + 12) return Parse::length;
        r.through = read_be(body, 8); r.count = static_cast<u16>(read_be(body + 8, 2));
        if (r.count > max_held || size != header_bytes + 12 + std::size_t(r.count) * 4) return Parse::length;
        if (read_be(body + 10, 2)) return Parse::payload;
        for (std::size_t i = 0; i < r.count; ++i) {
            r.held[i] = {static_cast<u16>(read_be(body + 12 + i * 4, 2)), static_cast<u16>(read_be(body + 14 + i * 4, 2))};
        }
    } else if (r.kind == Kind::probe) {
        if (size != header_bytes) return Parse::length;
    } else return Parse::type;
    const auto result = validate(r);
    if (result == Parse::ok) output = r;
    return result;
}
inline Parse encode(const Record& r, std::uint8_t* output, std::size_t capacity, std::size_t& written) {
    written = 0;
    auto result = validate(r);
    if (result != Parse::ok) return result;
    const std::size_t body = r.kind == Kind::key ? 8 : r.kind == Kind::state ? 12 + std::size_t(r.count) * 4 : 0;
    if (!output || capacity < header_bytes + body) return Parse::length;
    std::fill(output, output + header_bytes + body, std::uint8_t(0));
    output[0] = 1; write_be(output + 2, 2, header_bytes);
    write_be(output + 4, 8, r.generation); write_be(output + 12, 8, r.seq); write_be(output + 20, 8, r.capture);
    write_be(output + 28, 2, static_cast<u16>(r.kind)); write_be(output + 30, 2, body);
    auto* p = output + header_bytes;
    if (r.kind == Kind::key) {
        write_be(p, 2, r.key.page); write_be(p + 2, 2, r.key.usage); p[4] = r.down ? 1 : 0;
    } else if (r.kind == Kind::state) {
        write_be(p, 8, r.through); write_be(p + 8, 2, r.count);
        for (std::size_t i = 0; i < r.count; ++i) {
            write_be(p + 12 + i * 4, 2, r.held[i].page); write_be(p + 14 + i * 4, 2, r.held[i].usage);
        }
    }
    written = header_bytes + body;
    return Parse::ok;
}

enum class Admission { accepted, duplicate, stale, unauthenticated, generation };
template<std::size_t N> class SeenWindow {
    static_assert(N > 0, "Window must be nonempty");
    std::array<u64, N> tags_ {};
    std::array<bool, N> valid_ {};
    u64 generation_, high_ = 0;
    bool started_ = false;
public:
    explicit SeenWindow(u64 generation) : generation_(generation) {}
    Admission accept(bool authenticated, u64 generation, u64 seq) {
        if (!authenticated) return Admission::unauthenticated;
        if (generation != generation_) return Admission::generation;
        if (started_ && seq <= high_ && high_ - seq >= N) return Admission::stale;
        const auto slot = static_cast<std::size_t>(seq % N);
        if (valid_[slot] && tags_[slot] == seq) return Admission::duplicate;
        if (!started_ || seq > high_) { high_ = seq; started_ = true; }
        tags_[slot] = seq; valid_[slot] = true;
        return Admission::accepted;
    }
    constexpr std::size_t capacity() const { return N; }
    bool contains(u64 seq) const {
        if (!started_ || seq > high_ || high_-seq >= N) return false;
        const auto slot = static_cast<std::size_t>(seq % N);
        return valid_[slot] && tags_[slot] == seq;
    }
};

template<u64 Limit = std::numeric_limits<u64>::max()> class Sequence {
    u64 next_ = 0;
    bool exhausted_ = false;
public:
    bool take(u64& output) {
        if (exhausted_) return false;
        output = next_;
        if (next_ == Limit) exhausted_ = true;
        else ++next_;
        return true;
    }
};

// Exact floor(value*numerator/denominator), with its remainder. The fast
// path covers normal ppm ratios. The bounded fallback handles 128-bit products
// portably without floating-point time conversion or compiler-specific types.
inline bool scale_ratio_floor(u64 value, u64 numerator, u64 denominator, u64& output, u64& remainder) noexcept {
    if (!numerator || !denominator) return false;
    if (numerator == denominator) { output = value; remainder = 0; return true; }
    const auto max = std::numeric_limits<u64>::max();
    const auto whole = value / denominator;
    const auto part = value % denominator;
    if (whole > max / numerator) return false;
    const auto base = whole * numerator;
    u64 fraction = 0, residual = 0;
    if (part <= max / numerator) {
        const auto product = part * numerator;
        fraction = product / denominator;
        residual = product % denominator;
    } else {
        for (unsigned bit = 0; bit < 64; ++bit) {
            u64 carry = 0;
            if (residual >= denominator - residual) {
                residual -= denominator - residual; carry = 1;
            } else residual += residual;
            if ((numerator >> (63 - bit)) & 1) {
                if (residual >= denominator - part) {
                    residual -= denominator - part; ++carry;
                } else residual += part;
            }
            if (fraction > (max - carry) / 2) return false;
            fraction = fraction * 2 + carry;
        }
    }
    if (base > max - fraction) return false;
    output = base + fraction; remainder = residual;
    return true;
}

struct FrozenMapping {
    u64 client_anchor, host_anchor, reserve;
    u64 rate_numerator = 1, rate_denominator = 1;
    bool deadline(u64 capture, u64& output) const noexcept {
        const auto max = std::numeric_limits<u64>::max();
        if (host_anchor > max - reserve) return false;
        const u64 base = host_anchor + reserve;
        u64 scaled = 0, remainder = 0;
        const auto delta = capture >= client_anchor ? capture - client_anchor : client_anchor - capture;
        if (!scale_ratio_floor(delta,rate_numerator,rate_denominator,scaled,remainder)) return false;
        if (capture >= client_anchor) {
            // Ceil the complete affine deadline, including pre-anchor times.
            if (remainder) { if (scaled == max) return false; ++scaled; }
            if (base > max - scaled) return false;
            output = base + scaled;
        } else {
            if (scaled > base) return false;
            output = base - scaled;
        }
        return true;
    }
};
struct Scheduled { u64 capture, seq, deadline; };
inline bool later(Scheduled a, Scheduled b) {
    return a.capture > b.capture || (a.capture == b.capture && a.seq > b.seq);
}
template<std::size_t N> class Timeline {
    static_assert(N > 0, "Timeline must be nonempty");
    std::array<Scheduled, N> records_ {};
    std::size_t used_ = 0;
public:
    bool push(Scheduled record) {
        if (used_ == N) return false; // Caller must fault/teardown; never evict.
        records_[used_++] = record;
        std::push_heap(records_.begin(), records_.begin() + used_, later);
        return true;
    }
    bool pop_due(u64 now, Scheduled& output) {
        if (!used_ || records_[0].deadline > now) return false;
        std::pop_heap(records_.begin(), records_.begin() + used_, later);
        output = records_[--used_];
        return true;
    }
    const Scheduled* first() const { return used_ ? &records_[0] : nullptr; }
    std::size_t size() const { return used_; }
};

enum class Apply { transition, redundant, superseded, unsupported, closed };
// Logical ownership model only. It returns transitions/repair counts; it does
// not inject them. A backend/transactional error adapter is still required.
class KeyLedger {
    struct State { u64 seq = 0; bool seen = false, down = false; };
    std::array<State, 256> keys_ {};
    u64 watermark_ = 0;
    bool snapshot_seen_ = false;
    bool closed_ = false;
public:
    Apply apply(Key key, bool down, u64 seq) {
        if (closed_) return Apply::closed;
        if (key.page != 7 || key.usage < 4 || key.usage >= keys_.size()) return Apply::unsupported;
        auto& state = keys_[key.usage];
        if ((snapshot_seen_ && seq <= watermark_) || (state.seen && seq <= state.seq)) return Apply::superseded;
        state.seq = seq; state.seen = true;
        if (state.down == down) return Apply::redundant;
        state.down = down;
        return Apply::transition;
    }
    // Returns -1 on invalid/stale input without mutation. Desired state is
    // reconciled only when the timeline owner dispatches this snapshot.
    int reconcile(const Record& snapshot) {
        if (closed_ || snapshot.kind != Kind::state || validate(snapshot) != Parse::ok ||
            (snapshot_seen_ && snapshot.through <= watermark_)) return -1;
        std::array<bool, 256> desired {};
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            if (snapshot.held[i].usage < 4 || snapshot.held[i].usage >= desired.size()) return -1;
            desired[snapshot.held[i].usage] = true;
        }
        int repairs = 0;
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            auto& state = keys_[i];
            if (state.seen && state.seq > snapshot.through) continue;
            if (state.down != desired[i]) ++repairs;
            state.down = desired[i]; state.seq = snapshot.through; state.seen = true;
        }
        watermark_ = snapshot.through; snapshot_seen_ = true;
        return repairs;
    }
    bool held(u16 usage) const { return usage < keys_.size() && keys_[usage].down; }
    std::size_t disconnect() {
        std::size_t repairs = 0;
        for (auto& key : keys_) { repairs += key.down ? 1 : 0; key.down = false; }
        closed_ = true;
        return repairs; // Session object must be discarded, not reused.
    }
};
} // namespace steady
