#pragma once
#include <algorithm>
#include <bit>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dingosdk {
// XInput button bits, plus two synthetic trigger bits (above the dead zone).
inline constexpr std::uint32_t controller_button_mask = 0x3f3ff;
// Single keys retain their saved format. Chords pack up to five sorted key IDs
// into the remaining 30 bits of a distinct tag in the same profile field.
inline constexpr std::uint32_t keyboard_binding_tag = 0x80000000u;
inline constexpr std::uint32_t keyboard_chord_tag = 0x40000000u;
// Yes and No in a server's vote until the player binds them otherwise: F1 and F2.
inline constexpr std::uint32_t default_vote_yes_binding = keyboard_binding_tag | 0x70u, default_vote_no_binding = keyboard_binding_tag | 0x71u;
inline constexpr std::uint8_t keyboard_key_id(std::uint32_t key) noexcept {
    if (key >= '0' && key <= '9') return static_cast<std::uint8_t>(key - '0' + 1);
    if (key >= 'A' && key <= 'Z') return static_cast<std::uint8_t>(key - 'A' + 11);
    if (key == 0x20) return 37; // Space
    if (key >= 0x70 && key <= 0x7b) return static_cast<std::uint8_t>(key - 0x70 + 38); // F1-F12
    if (key == 0x11) return 50; // Ctrl
    if (key == 0x10) return 51; // Shift
    if (key == 0x12) return 52; // Alt
    return 0;
}
inline constexpr std::uint8_t keyboard_key_from_id(std::uint32_t id) noexcept {
    if (id >= 1 && id <= 10) return static_cast<std::uint8_t>('0' + id - 1);
    if (id >= 11 && id <= 36) return static_cast<std::uint8_t>('A' + id - 11);
    if (id == 37) return 0x20;
    if (id >= 38 && id <= 49) return static_cast<std::uint8_t>(0x70 + id - 38);
    if (id == 50) return 0x11;
    if (id == 51) return 0x10;
    if (id == 52) return 0x12;
    return 0;
}
inline constexpr bool bindable_keyboard_key(std::uint32_t key) noexcept {
    return keyboard_key_id(key) != 0;
}
inline constexpr bool keyboard_binding(std::uint32_t value) noexcept {
    if ((value & ~0xffu) == keyboard_binding_tag) return bindable_keyboard_key(value & 0xffu);
    if ((value & 0xc0000000u) != keyboard_chord_tag) return false;
    unsigned count = 0, previous = 0;
    bool ended = false;
    for (unsigned slot = 0; slot < 5; ++slot) {
        const unsigned id = (value >> (slot * 6)) & 0x3fu;
        if (!id) { ended = true; continue; }
        if (ended || !keyboard_key_from_id(id) || id <= previous) return false;
        previous = id;
        ++count;
    }
    return count >= 2;
}
inline std::uint32_t keyboard_binding_from_keys(const std::array<std::uint64_t, 4>& keys) noexcept {
    std::uint32_t value = keyboard_chord_tag;
    unsigned count = 0;
    for (unsigned id = 1; id <= 52; ++id) {
        const auto key = keyboard_key_from_id(id);
        if (!(keys[key / 64] & (std::uint64_t{1} << (key % 64)))) continue;
        if (count == 5) return 0;
        value |= id << (count++ * 6);
    }
    if (count == 1) {
        const auto id = value & 0x3fu;
        return keyboard_binding_tag | keyboard_key_from_id(id);
    }
    return count >= 2 ? value : 0;
}
inline constexpr bool valid_controller_combo(std::uint32_t value) noexcept {
    return (value & ~controller_button_mask) == 0;
}
inline constexpr bool valid_action_binding(std::uint32_t value) noexcept {
    return valid_controller_combo(value) || keyboard_binding(value);
}
// Button names shown to the player. Controller chords keep their XInput bits.
enum class ControllerStyle : std::uint8_t { xbox, dualshock4, dualsense };
struct ControllerInput {
    bool available{};
    std::uint32_t buttons{};
    // Identifies the connected source set; zero means unavailable. Any change
    // (a pad connecting, disconnecting or switching source) resets latches.
    unsigned device{};
    ControllerStyle style{};
    std::array<std::uint64_t, 4> keys{};
};
// Switches a player can put on a button or key: each runs a console command when pressed.
// `key` is its name in the profile, `name` what "bind <name>" takes.
struct ActionBind {
    std::string_view key, name, label, command;
};
inline constexpr std::array<ActionBind, 8> action_binds{{
    {"first_person", "firstperson", "First person", "firstperson toggle"},
    {"hide_hud", "hidehud", "Hide HUD", "hideui toggle"},
    {"voice_chat", "voicechat", "Voice chat on / off", "mp voice-chat toggle"},
    {"time_of_day", "tod", "Next time of day", "tod next"},
    {"no_bail", "nobail", "No Bail", "nobail toggle"},
    {"challenges", "challenges", "Show / hide challenges", "challenges toggle"},
    {"nametags", "nametags", "Player nametags", "mp nametags toggle"},
    {"nametag_dots", "nametagdots", "Player dots", "mp nametag-dots toggle"},
}};
struct ControllerBindingsModel {
    bool available{};
    bool freecam_controller{};
    std::uint32_t freecam_controller_combo{};
    std::uint32_t freecam_combo{};
    std::uint32_t tp_to_freecam_combo{};
    std::uint32_t noclip_combo{};
    std::uint32_t forward_velocity_combo{};
    std::uint32_t up_velocity_combo{};
    std::uint32_t offboard_up_velocity_combo{};
    std::uint32_t vote_yes_combo{}, vote_no_combo{}; // answering a dedicated server's vote
    std::array<std::uint32_t, action_binds.size()> action_combos{}; // by action_binds
    std::string status;
};
inline std::string controller_combo_label(std::uint32_t value, ControllerStyle style = ControllerStyle::xbox) {
    if (!value) return "Not bound";
    if (keyboard_binding(value)) {
        const auto key_name = [](std::uint32_t key) {
            if (key == 0x20) return std::string("Space");
            if (key == 0x11) return std::string("Ctrl");
            if (key == 0x10) return std::string("Shift");
            if (key == 0x12) return std::string("Alt");
            if (key >= 0x70) return "F" + std::to_string(key - 0x6fu);
            return std::string(1, static_cast<char>(key));
        };
        if ((value & 0xc0000000u) == keyboard_binding_tag) return key_name(value & 0xffu);
        std::string label;
        const auto append = [&](unsigned wanted) {
            for (unsigned slot = 0; slot < 5; ++slot) {
                const auto id = (value >> (slot * 6)) & 0x3fu;
                if (!id) break;
                if (id != wanted) continue;
                if (!label.empty()) label += " + ";
                label += key_name(keyboard_key_from_id(id));
            }
        };
        for (unsigned id = 50; id <= 52; ++id) append(id);
        for (unsigned id = 1; id < 50; ++id) append(id);
        return label;
    }
    const bool playstation = style != ControllerStyle::xbox;
    struct Button { std::uint32_t bit; const char* xbox; const char* playstation; };
    constexpr Button buttons[]{
        {0x100,"LB","L1"}, {0x200,"RB","R1"}, {0x10000,"LT","L2"}, {0x20000,"RT","R2"},
        {0x40,"L3","L3"}, {0x80,"R3","R3"},
        {0x1000,"A","Cross"}, {0x2000,"B","Circle"}, {0x4000,"X","Square"}, {0x8000,"Y","Triangle"},
        {1,"D-pad Up","D-pad Up"}, {2,"D-pad Down","D-pad Down"}, {4,"D-pad Left","D-pad Left"}, {8,"D-pad Right","D-pad Right"},
        {0x10,"Start","Options"}, {0x20,"Back",nullptr}};
    std::string label;
    for (const auto& button : buttons) if (value & button.bit) {
        if (!label.empty()) label += " + ";
        label += !playstation ? button.xbox : button.playstation ? button.playstation :
            style == ControllerStyle::dualsense ? "Create" : "Share";
    }
    return label;
}
// A held combo never repeats. Returning from menus, reconnecting, changing a
// binding, and changing level all require releasing the bound inputs first.
struct ControllerComboLatch {
    std::uint32_t combo{};
    unsigned device{};
    bool armed{};
    bool update(std::uint32_t binding, ControllerInput input, bool inhibited) noexcept {
        const bool keyboard = keyboard_binding(binding);
        const unsigned source = keyboard ? 0 : input.device;
        if (binding != combo || source != device) {
            combo = binding; device = source; armed = false;
        }
        if (inhibited || (!keyboard && !input.available) || !combo || !valid_action_binding(combo)) {
            armed = false; return false;
        }
        if (keyboard) {
            bool any = false, all = true;
            for (unsigned slot = 0; slot < 5; ++slot) {
                const auto key = (combo & 0xc0000000u) == keyboard_binding_tag ?
                    (slot ? 0u : combo & 0xffu) : keyboard_key_from_id((combo >> (slot * 6)) & 0x3fu);
                if (!key) break;
                const bool held = (input.keys[key / 64] & (std::uint64_t{1} << (key % 64))) != 0;
                any |= held; all &= held;
            }
            if (!any) armed = true;
            if (!armed || !all) return false;
            armed = false;
            return true;
        }
        const auto held = input.buttons & combo;
        if (!held) armed = true;
        if (!armed || held != combo) return false;
        armed = false;
        return true;
    }
};
// Record the largest simultaneous chord, not a union of sequential presses.
// Begin neutral so the input that opened Record cannot become the binding.
struct ControllerComboCapture {
    std::uint32_t chord{};
    unsigned device{};
    bool ready{};
    std::array<std::uint64_t, 4> keyboard_chord{};
    std::optional<std::uint32_t> update(ControllerInput input, bool allow_keyboard = false) noexcept {
        if (input.device != device || (!input.available && !allow_keyboard)) {
            chord = 0; ready = false; keyboard_chord = {}; device = input.device;
        }
        if (!input.available && !allow_keyboard) return {};
        const auto buttons = input.buttons & controller_button_mask;
        const bool keys_held = std::any_of(input.keys.begin(), input.keys.end(), [](auto word) { return word != 0; });
        if (!ready) { ready = buttons == 0 && (!allow_keyboard || !keys_held); return {}; }
        if (allow_keyboard && keys_held && !chord) {
            if (std::popcount(input.keys[0]) + std::popcount(input.keys[1]) +
                std::popcount(input.keys[2]) + std::popcount(input.keys[3]) >
                std::popcount(keyboard_chord[0]) + std::popcount(keyboard_chord[1]) +
                std::popcount(keyboard_chord[2]) + std::popcount(keyboard_chord[3])) keyboard_chord = input.keys;
        }
        if (std::any_of(keyboard_chord.begin(), keyboard_chord.end(), [](auto word) { return word != 0; })) {
            if (!keys_held) {
                const auto result = keyboard_binding_from_keys(keyboard_chord);
                keyboard_chord = {}; ready = false;
                if (result) return result;
            }
            return {};
        }
        if (!input.available) return {};
        if (std::popcount(buttons) > std::popcount(chord)) chord = buttons;
        if (!buttons && chord) {
            const auto result = chord; chord = 0; ready = false;
            return result;
        }
        return {};
    }
};
}
