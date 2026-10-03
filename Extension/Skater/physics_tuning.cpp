#include "physics_tuning.h"
#include "physics_presets.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include <windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace dingosdk::physics_tuning {
namespace {
using namespace addr::physics_tuning;
constexpr std::uintptr_t highest = memory::highest_user_address;
constexpr std::uint32_t max_points = 256;
using Refresh = void (*)(std::uintptr_t block);

struct Loader {
    std::mutex mutex;
    std::shared_ptr<const Model> model;
    std::string error;
    bool started{};
};
Loader &loader() { static auto *value = new Loader; return *value; }
std::shared_ptr<const Model> model(std::string *error = nullptr) {
    auto &l = loader();
    std::lock_guard lock(l.mutex);
    if (error) *error = l.error;
    return l.model;
}

struct Enforcement {
    bool seen{};                          // `differences` hold what enforce was last given
    std::vector<std::uint8_t> differences;
    std::optional<Values> target;         // the game's tuning plus `differences`
    bool refused{};                       // `differences` were malformed
    std::uintptr_t asset{};               // the copy of the asset written to
    std::optional<Values> own;            // its values from before
    std::uintptr_t entity{}, block{};     // the local skater's cached copy last refreshed
    std::uint64_t next_check{};
    std::size_t mismatched{};
    std::string status;
};
Enforcement &state() { static auto *value = new Enforcement; return *value; }
int contracts = -1; // -1 unchecked, 0 another build's code, 1 the known code

std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || object > highest - offset || !memory::peek(object + offset, value) ||
        value < 0x10000 || value > highest - 0x10000) return 0;
    return value;
}
bool write_bytes(std::uintptr_t address, const void *source, std::size_t size) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void *>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool contracts_match(std::uintptr_t base) noexcept {
    if (contracts < 0) {
        contracts = 1;
        for (const auto &contract : {asset_setter_contract, refresh_block_contract}) {
            std::array<unsigned char, 32> actual{};
            if (!memory::peek(base + contract.rva, actual) || actual != contract.bytes) contracts = 0;
        }
        if (!contracts)
            logging::log(logging::Level::warning, logging::Channel::runtime,
                         "Physics tuning sync is unavailable: this game build's tuning code is not the known one.");
    }
    return contracts == 1;
}
std::uintptr_t live_asset(std::uintptr_t base) noexcept {
    const auto asset = pointer(base + asset_global);
    if (!asset || asset > highest - asset_size || pointer(asset, asset_type_offset) != base + asset_type_info) return 0;
    return asset;
}
// The FloatCurve a slot points at (the pointer may carry a tag in bit 2) and its points.
struct LiveCurve {
    std::uintptr_t curve{}, points{};
    std::uint32_t count{};
};
bool locate(std::uintptr_t asset, std::uint16_t slot, LiveCurve &out) noexcept {
    std::uintptr_t tagged{};
    if (!memory::peek(asset + slot, tagged)) return false;
    out.curve = tagged & ~std::uintptr_t{4};
    if (out.curve < 0x10000 || out.curve > highest - 0x28 || !memory::peek(out.curve + curve_points_offset, out.points))
        return false;
    out.count = 0;
    if (!out.points) return true;
    if (out.points < 0x10000 || out.points > highest - max_points * curve_point_size || !memory::peek(out.points - 4, out.count))
        return false;
    out.count &= 0x7fffffff;
    return out.count <= max_points;
}
bool read_values(std::uintptr_t asset, const Model &m, Values &out) {
    out.image.resize(asset_size);
    if (!memory::peek_bytes(asset, out.image.data(), asset_size)) return false;
    out.curves.assign(m.curve_slots.size(), std::nullopt);
    for (std::size_t i = 0; i < m.curve_slots.size(); ++i) {
        LiveCurve live;
        Curve curve;
        if (!locate(asset, m.curve_slots[i], live) || !memory::peek(live.curve + curve_min_offset, curve.min) ||
            !memory::peek(live.curve + curve_max_offset, curve.max))
            continue;
        curve.points.resize(std::size_t{live.count} * curve_point_size);
        if (live.count && !memory::peek_bytes(live.points, curve.points.data(), curve.points.size())) continue;
        out.curves[i] = std::move(curve);
    }
    return true;
}
struct Written {
    std::size_t values{}, curves{}, mismatched{};
    bool failed{};
};
// Writes `target` wherever the running game (`live`) differs. A curve keeps its own points
// when their number differs: the game owns that memory.
Written write_values(std::uintptr_t asset, const Model &m, const Values &live, const Values &target) noexcept {
    Written w;
    for (const auto &f : m.fields) {
        if (!std::memcmp(live.image.data() + f.offset, target.image.data() + f.offset, f.size)) continue;
        if (!write_bytes(asset + f.offset, target.image.data() + f.offset, f.size)) { w.failed = true; return w; }
        ++w.values;
    }
    for (std::size_t i = 0; i < m.curve_slots.size() && i < target.curves.size() && i < live.curves.size(); ++i) {
        const auto &want = target.curves[i];
        const auto &have = live.curves[i];
        if (!want || !have || *want == *have) continue;
        LiveCurve curve;
        if (have->points.size() != want->points.size() || !locate(asset, m.curve_slots[i], curve) ||
            std::size_t{curve.count} * curve_point_size != want->points.size()) {
            ++w.mismatched;
            continue;
        }
        if ((!want->points.empty() && !write_bytes(curve.points, want->points.data(), want->points.size())) ||
            !write_bytes(curve.curve + curve_min_offset, &want->min, sizeof(float)) ||
            !write_bytes(curve.curve + curve_max_offset, &want->max, sizeof(float))) {
            w.failed = true;
            return w;
        }
        ++w.curves;
    }
    return w;
}
// The local skater's cached copy of about 60 tuning values: skater entity +0x628 is its
// skater component, whose +0x70 is the physics controller.
std::uintptr_t skater_block(std::uintptr_t base, std::uintptr_t entity) noexcept {
    if (!entity || pointer(entity) != base + addr::engine::skater_entity_vtable) return 0;
    const auto component = pointer(entity, 0x628);
    const auto core = pointer(component, 0x70);
    if (!component || pointer(component) != base + addr::engine::skater_component_vtable ||
        !core || pointer(core) != base + addr::no_bail::bail_core_vtable)
        return 0;
    return pointer(core, core_tuning_block);
}
bool refresh(std::uintptr_t base, std::uintptr_t block) noexcept {
    __try {
        reinterpret_cast<Refresh>(base + refresh_block_contract.rva)(block);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

void prepare() noexcept {
    auto &l = loader();
    {
        std::lock_guard lock(l.mutex);
        if (l.started) return;
        l.started = true;
    }
    try {
        std::thread([] {
            const auto started = GetTickCount64();
            std::shared_ptr<const Model> result;
            std::string error;
            try {
                std::vector<wchar_t> exe(32768);
                const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
                if (!length || length >= exe.size()) throw std::runtime_error("the game's folder is unknown");
                result = std::make_shared<const Model>(read_game_tuning(std::filesystem::path(exe.data()).parent_path()));
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Physics tuning: read the game's own ({} values, {} curves) in {} ms.", result->fields.size(),
                             result->curve_slots.size(), GetTickCount64() - started);
            } catch (const std::exception &failure) {
                error = failure.what();
                logging::log(logging::Level::warning, logging::Channel::runtime,
                             "Physics tuning sync is unavailable: {}.", error);
            }
            auto &l = loader();
            std::lock_guard lock(l.mutex);
            l.model = std::move(result);
            l.error = std::move(error);
        }).detach();
    } catch (...) {
        std::lock_guard lock(l.mutex);
        l.error = "could not start reading the game's tuning";
    }
}

std::optional<Encoded> local_differences(std::uintptr_t base, std::size_t limit) {
    if (!base || !contracts_match(base)) return std::nullopt;
    const auto m = model();
    const auto asset = live_asset(base);
    Values live;
    if (!m || !asset || !read_values(asset, *m, live)) return std::nullopt;
    return encode_differences(*m, live, limit);
}

void enforce(std::uintptr_t base, std::uintptr_t entity, std::span<const std::uint8_t> differences) noexcept {
    try {
        auto &e = state();
        const auto now = GetTickCount64();
        const bool same = e.seen && std::ranges::equal(differences, e.differences);
        if (same && now < e.next_check) return;
        e.next_check = now + 1000;
        if (!same) {
            e.seen = true;
            e.differences.assign(differences.begin(), differences.end());
            e.target.reset();
            e.refused = false;
        }
        if (!base || !contracts_match(base)) {
            e.status = "Physics tuning sync is unavailable for this game build.";
            return;
        }
        std::string error;
        const auto m = model(&error);
        if (!m) {
            e.status = error.empty() ? "Reading the game's physics tuning..." : "Physics tuning sync is unavailable: " + error + ".";
            return;
        }
        if (!e.target && !e.refused) {
            e.target = apply_differences(*m, e.differences);
            e.refused = !e.target;
            if (e.refused)
                logging::log(logging::Level::warning, logging::Channel::runtime,
                             "Physics tuning: the host's tuning ({} bytes) is malformed; skating with your own.", e.differences.size());
        }
        const auto asset = live_asset(base);
        Values live;
        if (!asset || !read_values(asset, *m, live)) {
            e.status = "Waiting for the game's physics tuning.";
            return;
        }
        // A new copy of the asset (a level load) holds the player's own values.
        if (asset != e.asset) {
            e.asset = asset;
            e.own = live;
            e.block = 0;
        }
        // Refused differences: back to the player's own.
        const auto written = write_values(asset, *m, live, e.target ? *e.target : *e.own);
        if (written.failed) {
            e.status = "Physics tuning could not be written.";
            return;
        }
        const auto block = skater_block(base, entity);
        if (block && (written.values || written.curves || block != e.block) && refresh(base, block)) {
            e.block = block;
            e.entity = entity;
        }
        if (written.values || written.curves || written.mismatched != e.mismatched)
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Physics tuning: set {} values and {} curves to the session's tuning{}.", written.values, written.curves,
                         written.mismatched ? ", " + std::to_string(written.mismatched) + " curves keep your own (another number of points)" : "");
        e.mismatched = written.mismatched;
        e.status = e.refused ? "The host's physics tuning is malformed; you skate with your own."
                 : e.differences.empty() ? "You skate with the game's physics tuning."
                                         : "You skate with the host's physics tuning.";
    } catch (...) {}
}

void release(std::uintptr_t base) noexcept {
    auto &e = state();
    if (!e.seen) return;
    try {
        const auto m = model();
        const auto asset = base && contracts == 1 ? live_asset(base) : 0;
        Values live;
        if (m && e.own && asset && asset == e.asset && read_values(asset, *m, live)) {
            const auto written = write_values(asset, *m, live, *e.own);
            if (written.values || written.curves) {
                if (const auto block = skater_block(base, e.entity)) refresh(base, block);
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Physics tuning: put back your own ({} values, {} curves).", written.values, written.curves);
            }
        }
    } catch (...) {}
    e = Enforcement{};
}

std::string status() { return state().status; }
namespace live {
namespace {
constexpr std::uint64_t idle_after_ms = 2000; // the menu is not drawing
constexpr std::uint64_t poll_ms = 100;
constexpr std::size_t max_requests = 8192;
constexpr float max_magnitude = 1e6f;

struct Live {
    std::mutex mutex; // guards everything up to `reset_requested`
    std::shared_ptr<const std::vector<Param>> params;
    std::vector<float> values;
    std::vector<std::string> presets;
    std::string status = "Reading the game's physics tuning...";
    bool ready{}, locked{}, presets_loaded{};
    std::size_t changed{};
    std::uint64_t viewed{};
    std::vector<std::pair<std::uint16_t, float>> requests;
    bool reset_requested{};
    // The client thread's alone.
    std::map<std::uint16_t, float> desired; // offset -> value, including ones set back to the game's
    std::uintptr_t asset{}, block{};
    std::uint64_t next{};
};
Live &ui() { static auto *value = new Live; return *value; }

float read_value(std::span<const std::uint8_t> image, const Field &field) noexcept {
    if (field.flag) return image[field.offset] ? 1.0f : 0.0f;
    float value;
    std::memcpy(&value, image.data() + field.offset, sizeof(value));
    return value;
}
float read_value(std::span<const std::uint8_t> image, const Param &param) noexcept {
    if (param.flag) return image[param.offset] ? 1.0f : 0.0f;
    float value;
    std::memcpy(&value, image.data() + param.offset, sizeof(value));
    return value;
}
std::shared_ptr<const std::vector<Param>> make_params(const Model &m) {
    auto result = std::make_shared<std::vector<Param>>();
    for (const auto &field : m.fields) {
        if (!(field.real || field.flag) || field.name.empty() || std::size_t{field.offset} + field.size > m.image.size()) continue;
        Param param;
        param.path = field.name;
        const auto dot = field.name.find('.');
        param.group = dot == std::string::npos ? "Other" : field.name.substr(0, dot);
        param.name = dot == std::string::npos ? field.name : field.name.substr(dot + 1);
        param.offset = field.offset;
        param.flag = field.flag;
        param.game = read_value(m.image, field);
        if (std::isfinite(param.game)) result->push_back(std::move(param));
    }
    return result;
}
const std::filesystem::path &presets_directory() {
    static const std::filesystem::path directory = [] {
        std::filesystem::path result;
        PWSTR local{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local)))
            result = std::filesystem::path(local) / L"ReSkate" / L"physics_presets";
        CoTaskMemFree(local);
        return result;
    }();
    return directory;
}
void refresh_presets() {
    auto &l = ui();
    auto names = presets_directory().empty() ? std::vector<std::string>{} : presets::list(presets_directory());
    std::lock_guard lock(l.mutex);
    l.presets = std::move(names);
    l.presets_loaded = true;
}
bool usable(const Snapshot &s) noexcept {
    return s.ready && !s.locked && s.params && s.values.size() == s.params->size();
}
} // namespace

Snapshot snapshot() {
    auto &l = ui();
    bool load_names{};
    {
        std::lock_guard lock(l.mutex);
        l.viewed = GetTickCount64();
        load_names = !l.presets_loaded;
    }
    if (load_names) refresh_presets();
    std::lock_guard lock(l.mutex);
    return {l.ready, l.locked, l.status, l.params, l.values, l.presets, l.changed};
}

void set(std::uint16_t offset, float value) {
    auto &l = ui();
    std::lock_guard lock(l.mutex);
    if (!l.requests.empty() && l.requests.back().first == offset) l.requests.back().second = value;
    else if (l.requests.size() < max_requests) l.requests.emplace_back(offset, value);
}

void reset_all() {
    auto &l = ui();
    std::lock_guard lock(l.mutex);
    l.requests.clear();
    l.reset_requested = true;
}

bool save_preset(std::string_view name) {
    if (presets_directory().empty() || !presets::valid_name(name)) return false;
    const auto s = snapshot();
    if (!usable(s)) return false;
    presets::Values values;
    for (std::size_t i = 0; i < s.params->size(); ++i) {
        const auto &param = (*s.params)[i];
        if (s.values[i] != param.game) values[param.path] = s.values[i];
    }
    if (!presets::save(presets_directory(), name, values)) return false;
    refresh_presets();
    return true;
}

bool load_preset(std::string_view name) {
    if (presets_directory().empty()) return false;
    const auto loaded = presets::load(presets_directory(), name);
    if (!loaded) return false;
    const auto s = snapshot();
    if (!usable(s)) return false;
    for (std::size_t i = 0; i < s.params->size(); ++i) {
        const auto &param = (*s.params)[i];
        const auto found = loaded->find(param.path);
        float target = found != loaded->end() ? found->second : param.game;
        if (param.flag) target = target >= 0.5f ? 1.0f : 0.0f;
        if (target != s.values[i]) set(param.offset, target);
    }
    return true;
}

bool delete_preset(std::string_view name) {
    if (presets_directory().empty() || !presets::remove(presets_directory(), name)) return false;
    refresh_presets();
    return true;
}

void tick(std::uintptr_t base, std::uintptr_t entity) noexcept {
    try {
        auto &l = ui();
        const auto now = GetTickCount64();
        std::vector<std::pair<std::uint16_t, float>> requests;
        bool reset{}, viewed{};
        {
            std::lock_guard lock(l.mutex);
            requests.swap(l.requests);
            reset = std::exchange(l.reset_requested, false);
            viewed = now - l.viewed < idle_after_ms;
        }
        const bool pending = !requests.empty() || reset;
        // Nothing is looking at the values and nothing is being kept: cost nothing.
        if (!pending && !viewed && l.desired.empty()) return;
        if (!pending && now < l.next) return;
        l.next = now + poll_ms;
        prepare();
        const auto unavailable = [&](std::string text) {
            std::lock_guard lock(l.mutex);
            l.ready = l.locked = false;
            l.status = std::move(text);
        };
        if (!base || !contracts_match(base)) {
            unavailable("Physics tuning is unavailable: this game build's tuning code is not the known one.");
            return;
        }
        std::string error;
        const auto m = model(&error);
        if (!m) {
            unavailable(error.empty() ? "Reading the game's physics tuning..." : "Physics tuning is unavailable: " + error + ".");
            return;
        }
        {
            std::lock_guard lock(l.mutex);
            if (!l.params) l.params = make_params(*m);
        }
        const auto asset = live_asset(base);
        if (!asset) {
            unavailable("Waiting for the game's physics tuning. Load into a world first.");
            return;
        }
        std::vector<std::uint8_t> image(asset_size);
        if (!memory::peek_bytes(asset, image.data(), image.size())) {
            unavailable("The game's physics tuning could not be read.");
            return;
        }
        const auto find = [&](std::uint16_t offset) -> const Field * {
            const auto it = std::ranges::lower_bound(m->fields, offset, {}, &Field::offset);
            return it != m->fields.end() && it->offset == offset && (it->real || it->flag) ? &*it : nullptr;
        };
        // A host's tuning is enforced on this guest: stand aside (the menu says so).
        const bool locked = state().seen;
        bool wrote{};
        if (!locked) {
            if (reset)
                for (auto &[offset, value] : l.desired)
                    if (const auto *field = find(offset)) value = read_value(m->image, *field);
            for (const auto &[offset, requested] : requests) {
                const auto *field = find(offset);
                if (!field || !std::isfinite(requested) || std::fabs(requested) > max_magnitude) continue;
                l.desired[offset] = field->flag ? (requested >= 0.5f ? 1.0f : 0.0f) : requested;
            }
            // A level load brings a new copy of the asset: ours go back onto it below.
            if (asset != l.asset) { l.asset = asset; l.block = 0; }
            for (const auto &[offset, value] : l.desired) {
                const auto *field = find(offset);
                if (!field || read_value(image, *field) == value) continue;
                std::array<std::uint8_t, 4> bytes{};
                if (field->flag) bytes[0] = static_cast<std::uint8_t>(value != 0.0f ? 1 : 0);
                else std::memcpy(bytes.data(), &value, sizeof(value));
                if (!write_bytes(asset + offset, bytes.data(), field->size)) {
                    unavailable("The game's physics tuning could not be written.");
                    return;
                }
                std::memcpy(image.data() + offset, bytes.data(), field->size);
                wrote = true;
            }
            // The skater keeps a cached copy of about 60 of the values: refresh it after a change
            // and when it is a new one (a respawn).
            const auto block = skater_block(base, entity);
            if (block && (wrote || (!l.desired.empty() && block != l.block)) && refresh(base, block)) l.block = block;
            // Back at the game's own value everywhere: nothing left to keep.
            std::erase_if(l.desired, [&](const auto &kept) {
                const auto *field = find(kept.first);
                return field && kept.second == read_value(m->image, *field) && read_value(image, *field) == kept.second;
            });
        }
        std::lock_guard lock(l.mutex);
        l.ready = true;
        l.locked = locked;
        l.status.clear();
        const auto &params = *l.params;
        l.values.resize(params.size());
        l.changed = 0;
        for (std::size_t i = 0; i < params.size(); ++i) {
            l.values[i] = read_value(image, params[i]);
            if (l.values[i] != params[i].game) ++l.changed;
        }
    } catch (...) {}
}
} // namespace live
} // namespace dingosdk::physics_tuning
