#include "score_hud.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace dingosdk::tricks {
namespace {
namespace menu_data = multiplayer::menu_data;
using Address = std::uintptr_t;
using Clock = std::chrono::steady_clock;

// ScoringHUDViewModel's fields, by the hashes of their names in the game's own type data
// (ui/features/hud/scoring/ScoringHUDViewModel; HudWidgetActive 0x3aa2f739 and ExtraInfoStyle
// 0x0426783f are the ones hud_corner.h already uses).
struct Watched {
    const char* name;
    std::uint32_t hash;
};
enum : std::size_t { line_score, is_slam, multiplier, action_active, sequence_header, sequence_descriptor, sequence_state };
constexpr std::array<Watched, 7> watched{{
    {"LineScore", 1086322279u},
    {"IsSlam", 2943226809u},
    {"Multiplier", 1782700230u},
    {"ActionActive", 2058053468u},
    // LabelViewModels inside the model: each one's Text (a string, the game's text value) first.
    {"SequenceHeader", 3018447676u},
    {"SequenceDescriptor", 3894822438u},
    {"SequenceState", 2134799591u},
}};
constexpr std::size_t value_bytes = 16; // read per field

struct Bound {
    bool present{};
    std::uint16_t offset{};
    unsigned size{}, kind{};
    std::array<unsigned char, value_bytes> now{};
    bool read{};
};
struct Hud {
    Clock::time_point next_look;
    Address manager{};
    menu_data::Value model;
    std::array<Bound, watched.size()> fields{};
    bool warned{};
};
Hud hud; // client thread

// A score field's value: the game's widget logic keeps scores as floats (kind 19, as LineTimer
// and Multiplier); an integer reading is taken if the float one is not sensible.
float score_of(const Bound& field) {
    if (!field.present || !field.read || field.size != 4) return 0;
    float f{};
    std::int32_t i{};
    std::memcpy(&f, field.now.data(), 4);
    std::memcpy(&i, field.now.data(), 4);
    if (std::isfinite(f) && (f == 0 || (std::abs(f) > 1e-3f && std::abs(f) < 1e9f))) return f;
    return static_cast<float>(i);
}
bool flag_of(const Bound& field) { return field.present && field.read && field.now[0] != 0; }
// A label's text: the game's text value is a pointer to its characters. Printable text only, so a
// value that is something else reads as none.
std::string text_of(const Bound& field) {
    if (!field.present || !field.read || field.size < 8) return {};
    Address at{};
    std::memcpy(&at, field.now.data(), sizeof at);
    char text[96]{};
    const auto length = memory::peek_cstring(at, text, sizeof text);
    if (length <= 0) return {};
    for (std::ptrdiff_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(text[i]) < 0x20) return {};
    return std::string(text, static_cast<std::size_t>(length));
}
std::uint32_t enum_of(const Bound& field) {
    if (!field.present || !field.read || field.size < 4) return 0;
    std::uint32_t value{};
    std::memcpy(&value, field.now.data(), 4);
    return value;
}
// (What the sequence labels say as they change, into the log while manuals are worked out.)
void trace_sequence(const ScoreHud& now) {
    static std::string last;
    static int said{};
    const auto text = std::format("\"{}\" / \"{}\" state {}", now.header, now.descriptor, now.sequence_state);
    if (text == last || said >= 200) return;
    last = text;
    ++said;
    logging::write(logging::Level::info, logging::Channel::skater, "Score HUD sequence: " + text);
}

// Runs `use(context)` on the game's UI model registry under its model lock (as Hall of Meat's
// HUD code does). False while there is no registry.
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

bool bind_model(Hud& h, const menu_data::Context& context) {
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
    bool any = false;
    for (std::size_t n = 0; n < watched.size(); ++n) {
        auto& field = h.fields[n];
        field = {};
        try {
            const auto member = context.member(model.type, watched[n].hash);
            field.present = any = true;
            field.offset = member.offset;
            field.size = menu_data::size(member.type);
            field.kind = menu_data::kind(member.type);
            layout += std::format(" {}@+0x{:x}:{}b/k{}", watched[n].name, field.offset, field.size, field.kind);
        } catch (const std::exception&) {
            layout += std::format(" {}:missing", watched[n].name);
        }
    }
    if (!any) return false;
    h.model = model;
    logging::write(logging::Level::info, logging::Channel::skater, "Score HUD model found:" + layout);
    return true;
}
}

std::optional<ScoreHud> read_score_hud(std::uintptr_t image_base) noexcept {
    auto& h = hud;
    bool ok = false;
    try {
        with_ui_models(image_base, [&](const menu_data::Context& context) {
            if (!bind_model(h, context)) return;
            const auto at = context.address(h.model);
            for (auto& field : h.fields)
                field.read = field.present && memory::peek_bytes(at + field.offset, field.now.data(), std::min<unsigned>(field.size, value_bytes));
            ok = true;
        });
        h.warned = false;
    } catch (const std::exception& error) {
        h.model = {};
        if (!std::exchange(h.warned, true))
            logging::log(logging::Level::warning, logging::Channel::skater, "The score HUD cannot be read: {}", error.what());
    } catch (...) {
        h.model = {};
    }
    if (!ok) return std::nullopt;
    const auto& f = h.fields;
    ScoreHud result{flag_of(f[action_active]), flag_of(f[is_slam]), score_of(f[line_score]), score_of(f[multiplier]),
                    text_of(f[sequence_header]), text_of(f[sequence_descriptor]), enum_of(f[sequence_state])};
    trace_sequence(result);
    return result;
}

bool names_manual(const ScoreHud& now) {
    const auto says = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return text.find("manual") != std::string::npos;
    };
    return now.action && (says(now.header) || says(now.descriptor));
}
}
