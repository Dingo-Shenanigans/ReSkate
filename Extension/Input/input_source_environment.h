#pragma once
#include <cstdint>
namespace dingosdk::native_input_source::environment {
struct Facts {
    std::uint64_t now_ms{};
    std::uint32_t thread{};
    bool focused{}, gameplay{}, menu_closed{}, interference_clear{};
};
// Platform leaves run before acquiring the publication gate. Their revision
// witness is checked after acquisition; no external leaf runs while held.
Facts sample() noexcept;
}
