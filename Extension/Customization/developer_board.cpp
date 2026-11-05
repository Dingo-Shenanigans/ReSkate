#include "developer_board.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <cstring>

namespace dingosdk {
namespace {
void report_board(std::uintptr_t entity, std::uint64_t steam_id, std::string_view message, logging::Level level) noexcept {
    struct Report { std::uintptr_t entity{}; std::uint64_t at{}; std::uint32_t message{}; };
    static std::array<Report, 33> reports{};
    const auto now = GetTickCount64();
    const auto hash = developer_board_detail::material_hash(message);
    auto *entry = &reports.front();
    for (auto &candidate : reports) {
        if (candidate.entity == entity) { entry = &candidate; break; }
        if (candidate.at < entry->at) entry = &candidate;
    }
    if (entry->entity == entity && (entry->message == hash || now - entry->at < 5000)) return;
    *entry = {entity, now, hash};
    logging::log(level, logging::Channel::customization, "Board items (Steam {}, actor {:#x}): {}", steam_id, entity, message);
}
bool write_color(std::uintptr_t base, const developer_board_detail::Binding &binding, const developer_board_detail::Color &color) noexcept {
    __try {
        if (!binding.node) {
            const developer_board_detail::NativeColor value{color, 0, base + addr::native_cosmetics::color_rgb_type};
            // Let the engine allocate/copy/hash a missing typed parameter. Priority
            // zero allows later native appearance presets to replace this override.
            return reinterpret_cast<std::uint64_t (*)(std::uintptr_t, const developer_board_detail::ParameterKey *,
                                                       const developer_board_detail::NativeColor *, std::uint8_t)>(
                base + addr::native_cosmetics::set_shader_parameter)(binding.material, &binding.key, &value, 0) != 0;
        }
        std::memcpy(reinterpret_cast<void *>(binding.node + 0x10), color.data(), sizeof(color));
        *reinterpret_cast<std::uint8_t *>(binding.material + 0x2f6) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool publish_materials(std::uintptr_t base, std::uintptr_t item) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, int)>(base + addr::native_cosmetics::publish_materials)(item, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// What each of the board's cosmetics does for this player: deck, grip tape, trucks, wheels.
// Only a player on one of the backend's lists has their board coloured; the solid colours
// anyone may pick are for what they wear.
using Animations = std::array<developer_board_detail::ItemAnimation, developer_board_detail::part_count>;
Animations board_animations(std::uint64_t steam_id, const multiplayer::MarkStyles &styles) noexcept {
    const auto mark = multiplayer::identity_mark(steam_id);
    Animations out;
    if (!mark) return out;
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = developer_board_detail::item_animation(mark, styles[multiplayer::skater_mark_items + i]);
    return out;
}
bool any_on(const Animations &animations) noexcept {
    return std::any_of(animations.begin(), animations.end(), [](const auto &animation) { return animation.on; });
}
} // namespace
void update_developer_board(std::uintptr_t base, std::uintptr_t board, std::uint64_t steam_id,
                            std::uint64_t generation, DeveloperBoardState &state,
                            const multiplayer::MarkStyles &styles) noexcept {
    const auto animations = board_animations(steam_id, styles);
    const bool listed = any_on(animations);
    if (!board) { state = {}; return; }
    if (!listed && state.colors.empty()) return;
    try {
        auto read = [](std::uintptr_t address, void *out, std::size_t size) { return memory::peek_bytes(address, out, size); };
        std::array<unsigned char, addr::native_cosmetics::publish_materials_prefix.size()> publisher{};
        std::array<unsigned char, addr::native_cosmetics::set_shader_parameter_prefix.size()> setter{};
        if (!read(base + addr::native_cosmetics::publish_materials, publisher.data(), publisher.size()) ||
            publisher != addr::native_cosmetics::publish_materials_prefix ||
            !read(base + addr::native_cosmetics::set_shader_parameter, setter.data(), setter.size()) ||
            setter != addr::native_cosmetics::set_shader_parameter_prefix) {
            report_board(board, steam_id, "material functions differ from the supported build", logging::Level::warning);
            return;
        }
        const auto live = developer_board_detail::materials(read, base, board);
        bool published = true;
        auto write = [base](const auto &binding, const auto &color) { return write_color(base, binding, color); };
        auto publish = [base, &published](std::uintptr_t item) { if (!publish_materials(base, item)) published = false; };
        developer_board_detail::animate(state, live, board, generation, animations, GetTickCount64(), write, publish);
        // The colours of the parts that are on: found, and taken.
        const auto wanted = std::count_if(live.bindings.begin(), live.bindings.end(),
                                          [&](const auto &binding) { return animations[binding.part].on; });
        const auto taken = std::count_if(state.colors.begin(), state.colors.end(),
                                         [&](const auto &saved) { return animations[saved.binding.part].on; });
        if (!published) report_board(board, steam_id, "native material publication failed", logging::Level::warning);
        else if (listed && !live.pending && taken < wanted)
            report_board(board, steam_id, "waiting for the remaining board color overrides", logging::Level::warning);
        else if (listed && taken) report_board(board, steam_id, "animation active on the board", logging::Level::info);
    } catch (const std::exception &error) {
        report_board(board, steam_id, error.what(), logging::Level::warning);
    } catch (...) {}
}
void tick_local_developer_board(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    static DeveloperBoardState state;
    if (!ready) return;
    try {
        const auto social = multiplayer::steam_social_snapshot();
        const auto id = social && multiplayer::own_items_shown() ? social->local.id : 0;
        const auto styles = developer_hoodie_detail::own_styles.load();
        if (!any_on(board_animations(id, styles)) && state.colors.empty()) return;
        const auto local = multiplayer::capture_local(base, client, false);
        if (!local.ready) return;
        auto read = [](std::uintptr_t address, void *out, std::size_t size) { return memory::peek_bytes(address, out, size); };
        // Cosmetic changes can replace the board while retaining the skater.
        // Re-resolve the actual local association, rather than capture's cached board.
        const auto board = multiplayer::read_native_board(read, base, local.entity);
        update_developer_board(base, board.entity, id, local.context, state, styles);
    } catch (...) {}
}
} // namespace dingosdk
