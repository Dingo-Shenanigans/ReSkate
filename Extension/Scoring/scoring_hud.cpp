#include "scoring_hud.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Extension/HallOfMeat/hall_of_meat_hud.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <mutex>
#include <utility>

namespace dingosdk::scoring_hud {
namespace {
namespace corner = game::build::v20260929::hud_corner;
namespace data = multiplayer::menu_data;

struct State {
    std::uintptr_t base{};
    std::atomic<Style> style{Style::game};
    std::mutex mutex;
    overlay::ScoringHud view;
    std::chrono::steady_clock::time_point visible_until{};
    bool warned{};
};
State& state() { static auto* value = new State; return *value; }

Style decode_style(const Json& value) {
    if (!value.is_string()) return Style::game;
    const auto text = value.get<std::string>();
    if (text == "skate1") return Style::skate1;
    if (text == "skate2") return Style::skate2;
    if (text == "skate3") return Style::skate3;
    return Style::game;
}
const char* encode_style(Style value) {
    switch (value) {
    case Style::skate1: return "skate1";
    case Style::skate2: return "skate2";
    case Style::skate3: return "skate3";
    default: return "game";
    }
}

std::string text(const data::Context& context, data::Value model) {
    return context.text(context.field(model, corner::text_value), 128);
}

overlay::ScoringHud capture(const data::Context& context, Style style) {
    overlay::ScoringHud result;
    result.style = style;
    const auto roots = context.roots({corner::score_view_schema});
    if (roots.empty()) return result;
    const auto model = roots.front().model;
    result.active = data::read<bool>(context.address(context.field(model, corner::hud_widget_active)));
    result.line_score = text(context, context.field(model, corner::line_score));
    result.multiplier = text(context, context.field(model, corner::multiplier));

    const auto stack = context.field(model, corner::trick_history);
    const auto items = context.field(stack, corner::stack_items);
    unsigned count{}, stride{};
    (void)context.array(items, 16, count, stride);
    for (unsigned index = 0; index < count; ++index) {
        const auto item = context.element(items, index);
        const auto presenter = context.field(item, corner::item_content);
        const auto widget = data::read<data::Widget>(context.address(presenter));
        if (!widget.data.handle) continue;
        const auto type = context.type_of(widget.data.handle);
        if (!type) continue;
        const data::Value row{widget.data.handle, type};
        const auto name = text(context, context.field(row, corner::trick_name));
        if (name.empty()) continue;
        result.tricks.push_back({name, text(context, context.field(row, corner::trick_score))});
    }
    return result;
}
}

bool start(std::uintptr_t image_base) noexcept {
    auto& s = state();
    s.base = image_base;
    if (const auto saved = profile_runtime::local_value("ScoringHudStyle"))
        s.style.store(decode_style(*saved), std::memory_order_release);
    overlay::set_scoring_hud_feed(&view);
    logging::log(logging::Level::info, logging::Channel::ui, "Scoring HUD styles ready ({}).", encode_style(s.style.load()));
    return image_base != 0;
}

void on_client_tick() noexcept {
    auto& s = state();
    const auto selected = s.style.load(std::memory_order_acquire);
    const bool custom = selected != Style::game;
    hall_of_meat::hook_score_hud(custom);
    try {
        const auto ui = data::read<data::Address>(s.base + addr::engine::ui_manager);
        const auto manager = ui ? data::read<data::Address>(ui + 0x140) : 0;
        if (!manager || !game::native_data().models.value) {
            hall_of_meat::replace_score_hud(false);
            return;
        }
        const data::Context context(s.base, manager);
        game::ModelWriteLock lock(manager);
        auto next = capture(context, selected);
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard guard(s.mutex);
            const bool scoring = next.active || !next.tricks.empty();
            if (custom && scoring) {
                next.active = true;
                s.visible_until = now + std::chrono::milliseconds(2600);
            } else if (custom && now < s.visible_until && s.view.style == selected) {
                next = s.view;
                next.active = true;
            }
            s.view = std::move(next);
            hall_of_meat::replace_score_hud(custom && s.view.active);
        }
        s.warned = false;
    } catch (const std::exception& error) {
        hall_of_meat::replace_score_hud(false);
        if (!std::exchange(s.warned, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Scoring HUD capture unavailable: {}.", error.what());
    } catch (...) {
        hall_of_meat::replace_score_hud(false);
    }
}

Style style() noexcept { return state().style.load(std::memory_order_acquire); }

void set_style(Style value) noexcept {
    if (value < Style::game || value > Style::skate3) value = Style::game;
    state().style.store(value, std::memory_order_release);
    profile_runtime::set_local_values({{"ScoringHudStyle", encode_style(value)}});
}

overlay::ScoringHud view() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto result = s.view;
    result.style = s.style.load(std::memory_order_acquire);
    return result;
}
}
