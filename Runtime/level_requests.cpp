#include "runtime_internal.h"
#include "Engine/Game/World/location_travel.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/World/loading_screen.h"
#include <algorithm>
#include <cstdio>

namespace dingosdk::runtime::detail {
namespace {
using LocalRequest = void (*)(std::uintptr_t, const void*, const void*, std::uint32_t);
using DescriptionInit = void* (*)(void*);
using DescriptionDestroy = void (*)(void*);
using StringAssign = void (*)(void*, const char*, std::uint32_t);
bool registered(const dingosdk::WorldCatalog& catalog, const Request& request,
                bool allow_manifest_only) {
    if (!catalog.available) return false;
    for (const auto& level : catalog.levels) if (equals(level.asset, request.asset)) {
        if (level.manifest_only && !allow_manifest_only) return false;
        if (request.start.empty()) return true;
        for (const auto& start : level.start_points) if (equals(start.name, request.start)) return true;
    }
    return false;
}
}
bool registered_request(const dingosdk::WorldCatalog& catalog, const Request& request) {
    // A manifest declaration intentionally covers the one native omission we
    // can safely bridge: a detached LM destination. The base/root must still be
    // present in the game's live LevelDescription registry.
    return registered(catalog, request, false) && (request.lm_level.empty() ? request.lm_start.empty() :
        registered(catalog, Request{request.lm_level, request.lm_start, "", ""}, true));
}
void submit_local(std::uintptr_t base, std::uintptr_t client, const Request& request, std::uint8_t flag) {
    struct Description {
        alignas(8) std::array<unsigned char, 0x30> bytes{};
        DescriptionDestroy destroy;
        Description(std::uintptr_t image) : destroy(reinterpret_cast<DescriptionDestroy>(image + rt::level_description_destroy)) {
            reinterpret_cast<DescriptionInit>(image + rt::level_description_init)(bytes.data());
        }
        ~Description() { destroy(bytes.data()); }
    } description(base);
    const auto assign = reinterpret_cast<StringAssign>(base + engine::string_assign);
    assign(description.bytes.data() + 0x10, request.asset.c_str(), static_cast<std::uint32_t>(request.asset.size()));
    assign(description.bytes.data() + 8, request.start.c_str(), static_cast<std::uint32_t>(request.start.size()));
    assign(description.bytes.data() + 0x20, request.lm_level.c_str(), static_cast<std::uint32_t>(request.lm_level.size()));
    assign(description.bytes.data() + 0x18, request.lm_start.c_str(), static_cast<std::uint32_t>(request.lm_start.size()));
    description.bytes[0x2c] = flag;
    // The game's client-vtable +0x70 implementation selects SinglePlayer or a
    // local Hosted server, deep-copies the description and posts an owned message.
    dingosdk::loading_screen::prepare(client, request.asset, request.lm_level);
    reinterpret_cast<LocalRequest>(base + rt::local_level_request)(client, description.bytes.data(), nullptr, 0);
}
dingosdk::multiplayer::MapLoadResult load_multiplayer_map(std::string_view destination, bool submitted,
                                                        std::string& detail) {
    using Result = dingosdk::multiplayer::MapLoadResult;
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (!dingosdk::multiplayer::valid_map_destination(destination)) {
        detail = "The host supplied an invalid map destination.";
        return Result::failed;
    }
    if (r.observer_failed) {
        detail = "The native map loader is unavailable. Restart ReSkate before joining.";
        return Result::failed;
    }
    const auto split = destination.find('|');
    const auto root_asset = destination.substr(0, split), sub_asset = destination.substr(split + 1);
    const dingosdk::overlay::Level *root = nullptr, *sub = nullptr;
    for (const auto& level : r.model.levels) {
        if (dingosdk::multiplayer::map_hash(level.asset) == dingosdk::multiplayer::map_hash(root_asset))
            root = &level;
        if (!sub_asset.empty() &&
            dingosdk::multiplayer::map_hash(level.asset) == dingosdk::multiplayer::map_hash(sub_asset))
            sub = &level;
    }
    if (r.model.levels.empty() || r.requests.loading()) {
        detail = submitted ? "Loading the host's map..." : "Waiting for the native map loader...";
        return Result::waiting;
    }
    if (!root || !root->native_registered || (!sub_asset.empty() && !sub)) {
        detail = "The host's map is not installed or registered on this PC. Install the same map and join again.";
        return Result::missing;
    }
    if (submitted) {
        detail = "The host's map did not finish loading. " + r.load_result;
        return Result::failed;
    }
    if (!root->can_load || (sub && !sub->can_load) ||
        !r.requests.can_load(request_context(r), GetCurrentThreadId())) {
        detail = "Waiting for the native map loader...";
        return Result::waiting;
    }
    Request request{root->asset, "", sub ? sub->asset : "", ""};
    if (sub) {
        if (const auto* point = sub->automatic_start_point()) request.lm_start = *point;
        else if (!sub->native_registered && !sub->start_points.empty()) {
            detail = "This custom map needs a spawn point. Load it from Levels, then join again.";
            return Result::failed;
        }
    }
    if (!r.requests.enqueue(std::move(request), request_context(r), GetCurrentThreadId())) {
        detail = "Waiting for queued game work before loading the host's map...";
        return Result::waiting;
    }
    detail = "Loading the host's map...";
    return Result::queued;
}
bool queue_load(void*, const char* asset, const char* start, const char* lm_level, const char* lm_start,
                char* result, std::size_t size) {
    const auto multiplayer = dingosdk::multiplayer::model();
    if (multiplayer.server_admin && asset && lm_level) {
        // A dedicated server's admin moves the whole server; everyone loads it together.
        const bool sent = dingosdk::multiplayer::queue_command("server", std::string("map ") + asset + "|" + lm_level, "");
        if (size) std::snprintf(result, size, "%s", sent ? "Asked the server to change map." : "The request queue is busy.");
        return sent;
    }
    if (dingosdk::multiplayer_controls_level(multiplayer)) {
        if (size) std::snprintf(result, size, "Only the lobby host can change levels.");
        return false;
    }
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (!asset || !start || !lm_level || !lm_start || !r.requests.can_load(request_context(r), GetCurrentThreadId())) {
        if (size) std::snprintf(result, size, "The local loader is not ready.");
        return false;
    }
    bool found = false;
    for (const auto& level : r.model.levels)
        if (level.asset == asset && level.can_load && level.native_registered) {
        found = !*start;
        for (const auto& point : level.start_points) found = found || point == start;
    }
    bool sublevel_found = !*lm_level && !*lm_start;
    for (const auto& level : r.model.levels) if (level.asset == lm_level && level.can_load) {
        sublevel_found = !*lm_start;
        for (const auto& point : level.start_points) sublevel_found = sublevel_found || point == lm_start;
    }
    found = found && sublevel_found;
    if (!found) { if (size) std::snprintf(result, size, "Choose a registered level and start point."); return false; }
    if (!r.requests.enqueue(Request{asset, start, lm_level, lm_start}, request_context(r), GetCurrentThreadId())) return false;
    if (size) result[0] = 0; // The menu says "Loading level..." itself.
    return true;
}
bool queue_location_travel(const char* map) {
    const auto multiplayer = dingosdk::multiplayer::model();
    const auto target = dingosdk::travel_level_asset(map ? map : "");
    if (multiplayer.server_admin)
        return !target.empty() && dingosdk::multiplayer::queue_command(
            "server", "map Levels/Game/DingoLevel_Root/DingoLevel_Root|" + std::string(target), "");
    if (dingosdk::multiplayer_controls_level(multiplayer)) return false;
    if (target.empty()) return false;
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (!r.requests.can_load(request_context(r), GetCurrentThreadId())) return false;
    Request request;
    for (const auto& level : r.model.levels) {
        if (!level.can_load) continue;
        if (equals(level.asset, "Levels/Game/DingoLevel_Root/DingoLevel_Root")) request.asset = level.asset;
        if (equals(level.asset, std::string(target))) request.lm_level = level.asset;
    }
    if (request.asset.empty() || request.lm_level.empty()) return false;
    if (!r.requests.enqueue(std::move(request), request_context(r), GetCurrentThreadId())) return false;
    return true;
}
// The launcher's start map (Launcher/start_maps.h): a "load <asset> [start point]" at the front of
// RESKATE_STARTUP_COMMANDS. The map and its start point, or nothing.
std::optional<std::pair<std::string, std::string>> startup_map() {
    std::wstring value(4096, L'\0');
    const auto length = GetEnvironmentVariableW(L"RESKATE_STARTUP_COMMANDS", value.data(), 4096);
    if (!length || length >= 4096) return std::nullopt;
    std::string text;
    for (const auto character : value.substr(0, length)) {
        if (character == L';') break;
        text.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
    const auto trim = [](std::string piece) {
        const auto first = piece.find_first_not_of(" \t");
        if (first == std::string::npos) return std::string{};
        return piece.substr(first, piece.find_last_not_of(" \t") - first + 1);
    };
    text = trim(std::move(text));
    if (text.size() < 6 || !equals(text.substr(0, 5), "load ")) return std::nullopt;
    text = trim(text.substr(5));
    const auto space = text.find(' ');
    return std::pair{text.substr(0, space), space == std::string::npos ? std::string{} : trim(text.substr(space + 1))};
}
// The first level from the splash world: the launcher's start map when it is installed and can load
// now, else San Vansterdam. So the game opens in the start map, rather than in San Van and then the
// map (the startup commands' "load" for it is dropped).
bool queue_menu_bam() {
    auto& r = runtime();
    std::string name = "San Vansterdam";
    bool start_map{};
    {
        std::lock_guard lock(r.mutex);
        if (!r.menu_splash_ready || r.observer_failed) return false;
        if (r.menu_load_queued && r.requests.loading()) return true;
        if (!r.requests.can_load(request_context(r), GetCurrentThreadId())) return false;
        Request request;
        for (const auto& level : r.model.levels) {
            if (!level.can_load) continue;
            if (equals(level.asset, "Levels/Game/DingoLevel_Root/DingoLevel_Root")) request.asset = level.asset;
            if (equals(level.asset, "levels/game/BAM_LevelRoot/BAM_LevelRoot")) request.lm_level = level.asset;
        }
        if (const auto wanted = startup_map()) {
            for (const auto& level : r.model.levels) {
                if (!level.can_load || !equals(level.asset, wanted->first) || equals(level.asset, request.asset)) continue;
                // Its start point as the console's "load" picks one: the one asked for, its only one,
                // or none for a map the game knows. A custom map with several and none asked for is
                // left to the startup command, which says why.
                std::string start = wanted->second;
                if (start.empty())
                    if (const auto* automatic = level.automatic_start_point()) start = *automatic;
                const bool known = start.empty() ? level.native_registered || level.start_points.empty()
                                                 : std::find(level.start_points.begin(), level.start_points.end(), start) != level.start_points.end();
                if (!known) break;
                request.lm_level = level.asset;
                request.lm_start = start;
                name = level.display_name.empty() ? level.asset : level.display_name;
                start_map = true;
                break;
            }
        }
        if (request.asset.empty() || request.lm_level.empty()) return false;
        if (!r.requests.enqueue(std::move(request), request_context(r), GetCurrentThreadId())) return false;
        r.menu_load_queued = true;
        if (start_map) r.startup_map_taken = true;
    }
    // Once accepted, even a log allocation failure must not show the error modal.
    try {
        activity_line(dingosdk::ConsoleSource::level, "Starting " + name + ".");
        record(start_map ? "{\"event\":\"menu_entry_load_queued\",\"destination\":\"start_map\"}"
                         : "{\"event\":\"menu_entry_load_queued\",\"destination\":\"BAM\"}");
    } catch (...) {}
    return true;
}
}
