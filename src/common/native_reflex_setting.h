#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace edvr::native_reflex {

enum class Mode : uint8_t { Off, On, Boost };

// Older profiles shipped "auto". It follows the current default, Off,
// even before installer migration. Unrecognized words also resolve to Off.
constexpr Mode parseMode(std::string_view word) noexcept {
    const auto matches = [word](std::string_view expected) {
        if (word.size() != expected.size()) return false;
        for (std::size_t i = 0; i < word.size(); ++i) {
            const char c = word[i];
            if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != expected[i]) return false;
        }
        return true;
    };
    if (matches("on")) return Mode::On;
    if (matches("boost")) return Mode::Boost;
    return Mode::Off;
}

constexpr const char* modeWord(Mode mode) noexcept {
    switch (mode) {
        case Mode::On:    return "on";
        case Mode::Boost: return "boost";
        default:          return "off";
    }
}

}  // namespace edvr::native_reflex
