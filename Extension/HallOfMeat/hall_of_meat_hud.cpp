#include "hall_of_meat_hud.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Engine/Game/Build/20260929/ui_model.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace dingosdk::hall_of_meat {
namespace {
namespace corner = addr::hud_corner;
namespace menu_data = multiplayer::menu_data;
using Address = std::uintptr_t;
using Clock = std::chrono::steady_clock;
using Value = std::uint64_t; // a field's value bytes, as the model's write takes them
using ModelWrite = std::uint64_t (*)(Address manager, std::uint64_t handle, Address type, const void* value, std::uint8_t flag);

// Our stack item: counted beside the d-pad (above its priority, within what the stack counts).
constexpr std::uint32_t cover_key = 0x52534843; // "RSHC": apart from the game's own keys (0x8000....)
constexpr std::uint64_t cover_id = 0x5253484300000000ULL;
constexpr std::int32_t cover_priority = corner::dpad_priority + 1;
static_assert(cover_priority < corner::score_priority);

// The score HUD's fields that hold it down, their sizes and the values that do.
struct Switch {
    std::uint32_t field;
    std::size_t size;
    Value down;
};
constexpr std::array<Switch, 2> score_switches{{
    {corner::hud_widget_active, 1, 0},
    {corner::extra_info_style, 4, static_cast<std::uint32_t>(corner::extra_info_none)},
}};

// A score HUD field as the model write hook sees it, on whichever thread the game writes from.
struct Taken {
    std::atomic<std::uint64_t> handle{}; // the field's in the current model; 0 none
    std::atomic<bool> taken{};
    std::atomic<Value> meant{}; // what the game last wrote, to give back
};
struct Hook {
    std::atomic<ModelWrite> original{};
    void* target{};
    bool ready{};  // prepared
    std::atomic<bool> hooked{}; // and enabled (hook_hud)
    std::array<Taken, score_switches.size()> fields;
};
Hook& hook() {
    static auto* value = new Hook;
    return *value;
}

// The game's write of a taken field writes ours instead and keeps the game's.
std::uint64_t model_write(Address manager, std::uint64_t handle, Address type, const void* value, std::uint8_t flag) {
    auto& h = hook();
    if (handle && value) {
        for (std::size_t index = 0; index < score_switches.size(); ++index) {
            auto& field = h.fields[index];
            if (!field.taken.load(std::memory_order_acquire) || handle != field.handle.load(std::memory_order_acquire)) continue;
            Value meant{};
            std::memcpy(&meant, value, score_switches[index].size);
            field.meant.store(meant, std::memory_order_release);
            value = &score_switches[index].down;
            break;
        }
    }
    return h.original.load(std::memory_order_acquire)(manager, handle, type, value, flag);
}

bool start_model_write_hook(Address base) noexcept {
    auto& h = hook();
    const auto& contract = addr::ui_model::model_write;
    std::array<unsigned char, 32> code{};
    auto status = HookNotFound;
    if (memory::peek(base + contract.rva, code) && code == contract.bytes) {
        auto* target = reinterpret_cast<void*>(base + contract.rva);
        void* relay{};
        status = hook_prepare(target, reinterpret_cast<void*>(&model_write), &relay);
        if (status == HookOk && !relay) status = HookUnsupportedFunction;
        if (status == HookOk) {
            h.original.store(reinterpret_cast<ModelWrite>(relay), std::memory_order_release); // before the hook is live
            h.target = target;
        } else (void)hook_remove(target);
    }
    if (status != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::ui,
            "Hall of Meat leaves skate.'s score HUD as it is: hooking the UI model's write failed ({}).", hook_status_string(status));
        return false;
    }
    h.ready = true;
    return true;
}

// Client thread only.
struct Corner {
    Address base{};
    Address manager{};
    menu_data::Value stack;       // the d-pad's stack, once found
    menu_data::Value cover;       // our stack item, while the corner is hidden
    menu_data::Value score_model; // ScoringHUDViewModel, once found
    std::array<std::uint16_t, score_switches.size()> score_offsets{};
    Clock::time_point next_stack_scan, next_score_scan;
    bool warned{}, slip_logged{};
};
Corner& corner_state() {
    static auto* value = new Corner;
    return *value;
}

// The field's handle in the current model (0: none); a new one gives up what was taken, as a new
// model holds the game's own state.
void bind(std::size_t index, std::uint64_t handle) noexcept {
    auto& field = hook().fields[index];
    if (field.handle.load(std::memory_order_acquire) == handle) return;
    field.taken.store(false, std::memory_order_release);
    field.handle.store(handle, std::memory_order_release);
}
bool score_held() noexcept {
    const auto& fields = hook().fields;
    return std::any_of(fields.begin(), fields.end(), [](const Taken& field) { return field.taken.load(std::memory_order_acquire); });
}

// Runs `use(context)` on the game's UI model registry under its model lock, which the game's
// writers take too. False while there is no registry.
template <class Use> bool with_ui_models(Address base, Use&& use) {
    if (!base) return false;
    const auto ui = menu_data::read<Address>(base + addr::engine::ui_manager);
    const auto manager = ui ? menu_data::read<Address>(ui + 0x140) : 0;
    if (!manager) return false;
    const menu_data::Context context(base, manager);
    game::ModelWriteLock lock(manager);
    use(context);
    return true;
}

std::string asset_name(Address asset) { return asset ? menu_data::string(menu_data::read<Address>(asset + 0x18), 256) : std::string{}; }
std::string widget_of(const menu_data::Context& context, const menu_data::Value& item) {
    return asset_name(menu_data::read<Address>(context.address(context.path(item, {corner::item_content, corner::content_blueprint}))));
}
menu_data::Value items_of(const menu_data::Context& context, const menu_data::Value& stack) {
    return context.field(stack, corner::stack_items);
}
// Whether `next` says it is time to look again (at most once a second while something is not found).
bool time_to_look(Clock::time_point& next) {
    const auto now = Clock::now();
    if (now < next) return false;
    next = now + std::chrono::seconds(1);
    return true;
}

// A new registry holds the game's own state: our item and what was taken went with the old one.
void follow_registry(Corner& c, const menu_data::Context& context) {
    if (c.manager == context.manager) return;
    c.manager = context.manager;
    c.stack = {};
    c.cover = {};
    c.score_model = {};
    for (std::size_t index = 0; index < score_switches.size(); ++index) bind(index, 0);
}

// The stack the d-pad is in.
bool find_stack(Corner& c, const menu_data::Context& context) {
    if (c.stack.handle && context.type_of(c.stack.handle) == c.stack.type) return true;
    c.stack = {};
    if (!time_to_look(c.next_stack_scan)) return false;
    for (const auto& root : context.roots({corner::hud_view_schema})) {
        const auto stack = context.field(root.model, corner::dpad_scoring_stack);
        const auto items = items_of(context, stack);
        unsigned count{}, stride{};
        (void)context.array(items, 16, count, stride);
        for (unsigned index = 0; index < count; ++index)
            if (widget_of(context, context.element(items, index)) == corner::dpad_widget) {
                c.stack = stack;
                return true;
            }
    }
    return false;
}

// Puts our item into the d-pad's stack, or takes it out; the game's items stay as they are. Our
// item: empty content, our key, the stack's transition as a push gives it.
void hide_dpad(Corner& c, const menu_data::Context& context, bool hidden) {
    const auto field = items_of(context, c.stack);
    unsigned count{}, stride{};
    auto items = context.array(field, 16, count, stride);
    menu_data::require(!count || stride == menu_data::stack_item.size, "the stack's items differ");
    const auto key_at = context.member(context.type(menu_data::stack_item), corner::item_key).offset;
    unsigned ours = count;
    for (unsigned index = 0; index < count; ++index) {
        std::uint32_t key{};
        std::memcpy(&key, items.data() + std::size_t{index} * stride + key_at, sizeof key);
        if (key == cover_key) ours = index;
    }
    items.resize(std::size_t{count} * menu_data::stack_item.size);
    if (hidden && ours == count) {
        if (!c.cover.handle) {
            c.cover = context.create(menu_data::stack_item, cover_id);
            context.set(context.field(c.cover, corner::item_key), cover_key);
            context.set(context.field(c.cover, corner::item_priority), cover_priority);
            context.copy(context.field(c.cover, corner::item_transition),
                         context.address(context.field(c.stack, corner::default_transition)));
        }
        std::vector<std::byte> bytes(menu_data::stack_item.size);
        menu_data::require(memory::read_bytes(context.address(c.cover), bytes.data(), bytes.size()), "our stack item is unavailable");
        items.insert(items.end(), bytes.begin(), bytes.end());
        context.array(field, items, count + 1);
    } else if (!hidden && ours < count) {
        const auto begin = items.begin() + static_cast<std::ptrdiff_t>(std::size_t{ours} * stride);
        items.erase(begin, begin + stride);
        context.array(field, items, count - 1);
    }
    if (!hidden && c.cover.handle) {
        context.destroy(c.cover);
        c.cover = {};
    }
}

// ScoringHUDViewModel, its switches bound to the model's fields.
bool find_score(Corner& c, const menu_data::Context& context) {
    if (c.score_model.handle && context.type_of(c.score_model.handle) == c.score_model.type) return true;
    c.score_model = {};
    for (std::size_t index = 0; index < score_switches.size(); ++index) bind(index, 0);
    if (!time_to_look(c.next_score_scan)) return false;
    const auto roots = context.roots({corner::score_view_schema});
    if (roots.empty()) return false;
    const auto model = roots.front().model;
    for (std::size_t index = 0; index < score_switches.size(); ++index) {
        const auto member = context.member(model.type, score_switches[index].field);
        menu_data::require(menu_data::size(member.type) == score_switches[index].size, "a score HUD field's size differs");
        c.score_offsets[index] = member.offset;
        bind(index, context.field(model, score_switches[index].field).handle);
    }
    c.score_model = model;
    return true;
}

// Holds the score HUD down (each switch taken at its value that does), or gives it back.
void hold_score(Corner& c, const menu_data::Context& context, bool held) {
    for (std::size_t index = 0; index < score_switches.size(); ++index) {
        const auto& sw = score_switches[index];
        auto& field = hook().fields[index];
        Value now{};
        menu_data::require(memory::peek_bytes(context.address(c.score_model) + c.score_offsets[index], &now, sw.size),
                           "a score HUD field is unavailable");
        if (held) {
            const bool taken = field.taken.load(std::memory_order_acquire);
            if (taken && now != sw.down && !std::exchange(c.slip_logged, true))
                logging::write(logging::Level::warning, logging::Channel::ui,
                    "Hall of Meat: the game wrote the score HUD past the model write hook.");
            if (!taken) field.meant.store(now, std::memory_order_release); // what the game means until it writes otherwise
            field.taken.store(true, std::memory_order_release);
            if (now != sw.down) context.publish(context.field(c.score_model, sw.field), &sw.down);
        } else if (field.taken.exchange(false, std::memory_order_acq_rel)) {
            const auto meant = field.meant.load(std::memory_order_acquire); // what the game wrote while it was held
            if (now != meant) context.publish(context.field(c.score_model, sw.field), &meant);
        }
    }
}
} // namespace

void start_hud(std::uintptr_t base) noexcept {
    corner_state().base = base;
    (void)start_model_write_hook(base); // without it the d-pad still hides
}

bool hook_hud(bool on) noexcept {
    auto& h = hook();
    if (!h.ready) return !on; // nothing to hook
    if (h.hooked.load(std::memory_order_acquire) == on) return true;
    const auto status = on ? hook_enable(h.target) : hook_disable(h.target);
    if (status != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Hall of Meat could not {} its UI model write hook ({}).",
            on ? "enable" : "disable", hook_status_string(status));
        return false;
    }
    h.hooked.store(on, std::memory_order_release);
    return true;
}

bool hide_hud(bool hidden) noexcept {
    auto& c = corner_state();
    // Without the hook the score HUD is never taken, but what was taken is always given back.
    const bool hold = hidden && hook().hooked.load(std::memory_order_acquire);
    try {
        if (!hidden && !c.cover.handle && !score_held()) return true; // the game's own corner, untouched
        with_ui_models(c.base, [&](const menu_data::Context& context) {
            follow_registry(c, context);
            if (find_stack(c, context)) hide_dpad(c, context, hidden);
            if ((hold || score_held()) && find_score(c, context)) hold_score(c, context, hold);
            c.warned = false;
        });
    } catch (const std::exception& error) {
        c.stack = {};
        c.score_model = {};
        if (!std::exchange(c.warned, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Hall of Meat cannot hide skate.'s HUD corner: {}.", error.what());
    } catch (...) {
        c.stack = {};
        c.score_model = {};
    }
    return !c.cover.handle && !score_held();
}
}
