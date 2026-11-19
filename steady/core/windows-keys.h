// Physical keyboard-page identity -> existing Moonlight Windows key semantics.
// Conversion only: no layout translation, native injection or repeat synthesis.
#pragma once
#include "core.h"

namespace steady {
struct WindowsKey {
    u16 vk = 0;
    bool extended = false, normalized = true;
};
inline bool windows_key(Key key, WindowsKey& output) noexcept {
    if (key.page != 7) return false;
    WindowsKey r;
    const auto u = key.usage;
    if (u >= 4 && u <= 29) r.vk = static_cast<u16>(0x41 + u - 4);
    else if (u >= 30 && u <= 38) r.vk = static_cast<u16>(0x31 + u - 30);
    else if (u >= 58 && u <= 69) r.vk = static_cast<u16>(0x70 + u - 58);
    else if (u >= 104 && u <= 115) r.vk = static_cast<u16>(0x7C + u - 104);
    else if (u >= 89 && u <= 97) r.vk = static_cast<u16>(0x61 + u - 89);
    else switch (u) {
    case 39: r.vk = 0x30; break;
    case 40: r.vk = 0x0D; break;
    case 41: r.vk = 0x1B; break;
    case 42: r.vk = 0x08; break;
    case 43: r.vk = 0x09; break;
    case 44: r.vk = 0x20; break;
    case 45: r.vk = 0xBD; break;
    case 46: r.vk = 0xBB; break;
    case 47: r.vk = 0xDB; break;
    case 48: r.vk = 0xDD; break;
    case 49: r.vk = 0xDC; break;
    case 51: r.vk = 0xBA; break;
    case 52: r.vk = 0xDE; break;
    case 53: r.vk = 0xC0; break;
    case 54: r.vk = 0xBC; break;
    case 55: r.vk = 0xBE; break;
    case 56: r.vk = 0xBF; break;
    case 57: r.vk = 0x14; break;
    case 70: r.vk = 0x2C; r.extended = true; break;
    case 71: r.vk = 0x91; break;
    case 72: r.vk = 0x13; break;
    case 73: r.vk = 0x2D; r.extended = true; break;
    case 74: r.vk = 0x24; r.extended = true; break;
    case 75: r.vk = 0x21; r.extended = true; break;
    case 76: r.vk = 0x2E; r.extended = true; break;
    case 77: r.vk = 0x23; r.extended = true; break;
    case 78: r.vk = 0x22; r.extended = true; break;
    case 79: r.vk = 0x27; r.extended = true; break;
    case 80: r.vk = 0x25; r.extended = true; break;
    case 81: r.vk = 0x28; r.extended = true; break;
    case 82: r.vk = 0x26; r.extended = true; break;
    case 83: r.vk = 0x90; break;
    case 84: r.vk = 0x6F; r.extended = true; break;
    case 85: r.vk = 0x6A; break;
    case 86: r.vk = 0x6D; break;
    case 87: r.vk = 0x6B; break;
    case 88: r.vk = 0x0D; r.extended = true; break;
    case 98: r.vk = 0x60; break;
    case 99: r.vk = 0x6E; break;
    case 100: r.vk = 0xE2; break;
    case 101: r.vk = 0x5D; r.extended = true; break;
    case 116: r.vk = 0x2B; break;
    case 117: r.vk = 0x2F; break;
    case 119: r.vk = 0x29; break;
    case 133: r.vk = 0x6C; break;
    case 135: r.vk = 0xE2; r.normalized = false; break;
    case 137: r.vk = 0xDC; r.normalized = false; break;
    case 144: r.vk = 0x1C; break;
    case 145: r.vk = 0x1D; break;
    case 156: r.vk = 0x0C; break;
    case 224: r.vk = 0xA2; break;
    case 225: r.vk = 0xA0; break;
    case 226: r.vk = 0xA4; break;
    case 227: r.vk = 0x5B; r.extended = true; break;
    case 228: r.vk = 0xA3; r.extended = true; break;
    case 229: r.vk = 0xA1; break;
    case 230: r.vk = 0xA5; r.extended = true; break;
    case 231: r.vk = 0x5C; r.extended = true; break;
    default: return false;
    }
    output = r;
    return true;
}
inline bool windows_record(const Record& record) noexcept {
    if (validate(record) != Parse::ok) return false;
    WindowsKey converted;
    if (record.kind == Kind::key) return windows_key(record.key,converted);
    if (record.kind == Kind::state) {
        for (std::size_t i = 0; i < record.count; ++i)
            if (!windows_key(record.held[i],converted)) return false;
    }
    return true;
}
} // namespace steady
