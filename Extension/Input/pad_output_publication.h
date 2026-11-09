#pragma once
#include "Engine/Game/Input/pad_output.h"
#include "input_publication.h"
#include "native_input_source.h"

namespace dingosdk::native_input_source {
// Take the publication gate before the replacement mutex. Engine state reports
// mutations without calling back into an extension or taking the reverse order.
inline std::string write_pad_output(std::string_view owner, unsigned controller,
    std::optional<input::PadOutput> pad, std::uint64_t now) {
    PublicationScope publication;
    bool changed{};
    auto result = input::pad_outputs().write(owner, controller, pad, now, &changed);
    if (changed) publication_changed();
    return result;
}
inline std::optional<input::PadOutput> read_pad_output(unsigned controller,
    std::uint64_t now, bool allowed) {
    PublicationScope publication;
    bool changed{};
    auto result = input::pad_outputs().read(controller, now, allowed && native_menu_closed(now), &changed);
    if (changed) publication_changed();
    return result;
}
inline void release_pad_output(std::string_view owner) {
    PublicationScope publication;
    bool changed{};
    input::pad_outputs().release(owner, &changed);
    if (changed) publication_changed();
}
}
