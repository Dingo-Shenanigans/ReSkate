#include "developer_hoodie.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <cstring>
#include <format>

namespace dingosdk {
namespace {
// Report a changed failure/activation at most once per actor per five seconds.
// Cosmetic streaming is transient; no per-frame log output or cached writes.
void report_hoodie(std::uintptr_t entity, std::uint64_t steam_id, std::string_view message,
                    logging::Level level) noexcept {
    struct Report { std::uintptr_t entity{}; std::uint64_t at{}; std::uint32_t message{}; };
    static std::array<Report, 33> reports{};
    const auto now = GetTickCount64();
    const auto hash = developer_hoodie_detail::material_hash(message);
    auto *entry = &reports.front();
    for (auto &candidate : reports) {
        if (candidate.entity == entity) { entry = &candidate; break; }
        if (candidate.at < entry->at) entry = &candidate;
    }
    if (entry->entity == entity && (entry->message == hash || now - entry->at < 5000)) return;
    *entry = {entity, now, hash};
    logging::log(level, logging::Channel::customization, "Skater items (Steam {}, actor {:#x}): {}",
                 steam_id, entity, message);
}
// NativeValue is owned by this material. Preserve its padding, type, priority,
// flags and key, and set the same dirty byte as the native addEsVector setter.
bool write_color(const developer_hoodie_detail::Binding &binding, const developer_hoodie_detail::Color &color) noexcept {
    __try {
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
// What each of the skater's marked cosmetics does for this player: top, bottoms, shoes, ...
// No player (0: their items are hidden, or nobody is known) has nothing; anyone else at least
// their solid colours (item_animation).
using Animations = std::array<developer_hoodie_detail::ItemAnimation, developer_hoodie_detail::slots.size()>;
Animations skater_animations(std::uint64_t steam_id, const multiplayer::MarkStyles &styles) noexcept {
    Animations out;
    if (!steam_id) return out;
    const auto mark = multiplayer::identity_mark(steam_id);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = developer_hoodie_detail::item_animation(mark, styles[i]);
    return out;
}
bool any_on(const Animations &animations) noexcept {
    return std::any_of(animations.begin(), animations.end(), [](const auto &animation) { return animation.on; });
}
// Which cosmetics are coloured, and how many colours of each: what is equipped decides.
std::string coloured(const DeveloperHoodieState &state) {
    std::array<unsigned, developer_hoodie_detail::slots.size()> colors{};
    for (const auto &saved : state.colors) ++colors[saved.binding.part];
    std::string out;
    for (std::size_t i = 0; i < colors.size(); ++i)
        if (colors[i]) out += std::format("{}{} ({})", out.empty() ? "" : ", ", multiplayer::mark_item_names[i], colors[i]);
    return out.empty() ? "nothing equipped has a color to change" : "animation active on " + out;
}
} // namespace
void update_developer_hoodie(std::uintptr_t base, std::uintptr_t entity, std::uint64_t steam_id,
                             std::uint64_t generation, DeveloperHoodieState &state,
                             const multiplayer::MarkStyles &styles) noexcept {
    const auto animations = skater_animations(steam_id, styles);
    const bool listed = any_on(animations);
    if (!entity) { state = {}; return; }
    if (!listed && state.colors.empty()) return;
    try {
        auto read = [](std::uintptr_t address, void *out, std::size_t size) { return memory::peek_bytes(address, out, size); };
        std::array<unsigned char, addr::native_cosmetics::publish_materials_prefix.size()> prefix{};
        if (!read(base + addr::native_cosmetics::publish_materials, prefix.data(), prefix.size()) ||
            prefix != addr::native_cosmetics::publish_materials_prefix) {
            report_hoodie(entity, steam_id, "material publisher differs from the supported build", logging::Level::warning);
            state = {}; return;
        }
        const auto live = developer_hoodie_detail::materials(read, base, entity);
        bool published = true;
        auto publish = [base, &published](std::uintptr_t item) { if (!publish_materials(base, item)) published = false; };
        developer_hoodie_detail::animate(state, live, entity, generation, animations, GetTickCount64(), write_color, publish);
        if (!published) report_hoodie(entity, steam_id, "native material publication failed", logging::Level::warning);
        else if (listed && !live.pending) report_hoodie(entity, steam_id, coloured(state), logging::Level::info);
    } catch (const std::exception &error) {
        report_hoodie(entity, steam_id, error.what(), logging::Level::warning);
    } catch (...) {
        // Streaming/replacement may invalidate a controller. Never recover by
        // writing cached addresses, or let an optional cosmetic stop the session.
    }
}
void tick_local_developer_hoodie(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    static DeveloperHoodieState state;
    // Pausing/loading can temporarily make the local actor unavailable without
    // destroying it. Keep originals until a fresh ownership walk succeeds.
    if (!ready) return;
    try {
        const auto social = multiplayer::steam_social_snapshot();
        const auto id = social && multiplayer::own_items_shown() ? social->local.id : 0;
        const auto styles = developer_hoodie_detail::own_styles.load();
        if (!any_on(skater_animations(id, styles)) && state.colors.empty()) return;
        const auto local = multiplayer::capture_local(base, client, false);
        if (local.ready) update_developer_hoodie(base, local.entity, id, local.context, state, styles);
        else report_hoodie(client, id, local.detail, logging::Level::warning);
    } catch (...) {}
}
} // namespace dingosdk
