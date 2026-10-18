#include "domination_internal.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include <algorithm>
#include <format>

// Tricks read from skate.'s score HUD view model (domination_internal.h).
namespace dingosdk::domination::detail {
// ---- reading the score HUD --------------------------------------------------------------
// Runs `use(context)` on the game's UI model registry under its model lock (as Hall of Meat's
// HUD code does). False while there is no registry.
template <class Use> bool with_ui_models(Address base, Use &&use) {
    if (!base) return false;
    const auto ui = menu_data::read<Address>(base + addr::engine::ui_manager);
    const auto manager = ui ? menu_data::read<Address>(ui + 0x140) : 0;
    if (!manager) return false;
    const menu_data::Context context(base, manager);
    game::ModelWriteLock lock(manager);
    use(context);
    return true;
}
bool bind_model(Hud &h, const menu_data::Context &context) {
    if (h.manager != context.manager) {
        h.manager = context.manager;
        h.model = {};
    }
    if (h.model.handle && context.type_of(h.model.handle) == h.model.type) return true;
    h.model = {};
    const auto now = Clock::now();
    if (now < h.next_look) return false;
    h.next_look = now + std::chrono::seconds(1);
    const auto roots = context.roots({addr::hud_corner::score_view_schema});
    if (roots.empty()) return false;
    const auto model = roots.front().model;
    std::string layout;
    for (std::size_t n = 0; n < watched.size(); ++n) {
        auto &field = h.fields[n];
        field = {};
        try {
            const auto member = context.member(model.type, watched[n].hash);
            field.present = true;
            field.offset = member.offset;
            field.size = menu_data::size(member.type);
            field.kind = menu_data::kind(member.type);
            layout += std::format(" {}@+0x{:x}:{}b/k{}", watched[n].name, field.offset, field.size, field.kind);
        } catch (const std::exception &) {
            layout += std::format(" {}:missing", watched[n].name);
        }
    }
    h.model = model;
    say(logging::Level::info, "found the score HUD model:" + layout);
    return true;
}
// Reads every watched field into `now`. False without the model.
bool read_hud(State &s) {
    auto &h = s.hud;
    bool ok = false;
    try {
        with_ui_models(s.base, [&](const menu_data::Context &context) {
            if (!bind_model(h, context)) return;
            const auto at = context.address(h.model);
            for (auto &field : h.fields) {
                field.read = field.present && memory::peek_bytes(at + field.offset, field.now.data(), std::min<unsigned>(field.size, value_bytes));
            }
            ok = true;
        });
        h.warned = false;
    } catch (const std::exception &error) {
        h.model = {};
        if (!std::exchange(h.warned, true)) say(logging::Level::warning, std::string("cannot read the score HUD: ") + error.what());
    } catch (...) {
        h.model = {};
    }
    return ok;
}

} // namespace dingosdk::domination::detail
