#include "board_material.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <format>

namespace dingosdk::board_material {
namespace {
struct State {
    std::uintptr_t base{}, client{};
    std::vector<detail::Hold> holds;
    std::vector<detail::Saved> saved;
    std::optional<bool> functions; // the material publisher's bytes, once checked
    std::uint64_t reported{};
};
State& state() {
    static auto* value = new State; // what is held may still be out at exit
    return *value;
}
const auto peek = [](std::uintptr_t address, void* out, std::size_t size) { return memory::peek_bytes(address, out, size); };

bool write_value(std::uintptr_t material, std::uintptr_t node, const float* value, std::size_t bytes) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(node + addr::native_cosmetics::parameter_value), value, bytes);
        *reinterpret_cast<std::uint8_t*>(material + addr::native_cosmetics::material_changed) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool publish_materials(std::uintptr_t base, std::uintptr_t item) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int)>(base + addr::native_cosmetics::publish_materials)(item, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool functions_match(State& s) {
    if (!s.functions) {
        std::array<unsigned char, addr::native_cosmetics::publish_materials_prefix.size()> bytes{};
        s.functions = peek(s.base + addr::native_cosmetics::publish_materials, bytes.data(), bytes.size()) &&
                      bytes == addr::native_cosmetics::publish_materials_prefix;
        if (!*s.functions)
            logging::log(logging::Level::warning, logging::Channel::customization,
                         "Board parameters: the material publisher differs from the supported build; plugins cannot set them.");
    }
    return *s.functions;
}
// The local player's board as it is now (a change of cosmetics can replace it); none out of play.
std::optional<detail::Found> board(State& s) {
    if (!s.base || !s.client) return std::nullopt;
    try {
        const auto local = multiplayer::capture_local(s.base, s.client, false);
        if (!local.ready) return std::nullopt;
        const auto entity = multiplayer::read_native_board(peek, s.base, local.entity).entity;
        if (!entity) return std::nullopt;
        return detail::walk(peek, s.base, entity);
    } catch (const std::exception& error) {
        // (Once in a while: a board being built reads as unfinished for a few ticks.)
        if (const auto now = GetTickCount64(); now - s.reported > 10000) {
            s.reported = now;
            logging::log(logging::Level::debug, logging::Channel::customization, "Board parameters: {}", error.what());
        }
        return std::nullopt;
    }
}
} // namespace

std::optional<std::vector<Parameter>> read(Part part) {
    auto& s = state();
    const auto found = board(s);
    if (!found) return std::nullopt;
    std::vector<Parameter> out;
    // The first of the part's materials: another (a variant) holds the same keys.
    for (const auto& material : found->materials)
        if (material.part == part) {
            for (const auto& node : material.nodes) out.push_back(node.parameter);
            break;
        }
    return out;
}

std::string set(std::string_view owner, Part part, std::uint64_t key, const Colour& value, bool colour) {
    auto& s = state();
    const auto name = part_names[static_cast<std::size_t>(part)];
    const auto found = board(s);
    if (!found) return "there is no board in play";
    Kind kind{};
    if (auto why = detail::refusal(*found, part, key, value, colour, kind); !why.empty()) return why;
    if (!functions_match(s)) return "board parameters are unavailable with this game build";
    const auto held = std::find_if(s.holds.begin(), s.holds.end(), [&](const detail::Hold& h) { return h.part == part && h.key == key; });
    if (held != s.holds.end() && held->owner != owner) return std::format("{} holds the {}'s {:016x}", held->owner, name, key);
    if (held == s.holds.end() &&
        static_cast<std::size_t>(std::count_if(s.holds.begin(), s.holds.end(), [&](const detail::Hold& h) { return h.owner == owner; })) >= most_held)
        return std::format("a plugin holds at most {} board parameters", most_held);
    const Colour kept = colour ? value : Colour{value[0], 0, 0};
    if (held != s.holds.end()) held->value = kept;
    else s.holds.push_back({std::string(owner), part, key, kind, kept});
    return {};
}

void clear(std::string_view owner, std::optional<Part> part, std::optional<std::uint64_t> key) {
    auto& holds = state().holds;
    std::erase_if(holds, [&](const detail::Hold& h) { return h.owner == owner && (!part || h.part == *part) && (!key || h.key == *key); });
}

void tick(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        auto& s = state();
        s.base = base, s.client = client;
        if (s.holds.empty() && s.saved.empty()) return;
        if (!functions_match(s)) return;
        const auto found = board(s);
        if (!found) return;
        auto write = [](std::uintptr_t material, std::uintptr_t node, Kind kind, const Colour& value) {
            return write_value(material, node, value.data(), kind == Kind::colour ? sizeof(Colour) : sizeof(float));
        };
        auto publish = [&](std::uintptr_t item) { (void)publish_materials(s.base, item); };
        detail::apply(s.saved, s.holds, *found, write, publish);
    } catch (...) {}
}
} // namespace dingosdk::board_material
