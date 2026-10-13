#include "embedded_profile_defaults.h"
#include "profile_internal.h"
#include "database_codec.h"
#include "local_profile.h"
#include "profile_update.h"
#include "Engine/Game/World/location_travel.h"
#include "Engine/Vfs/content_catalogs.h"
#include <algorithm>
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace dingosdk::profile {
using namespace detail;
namespace detail {
using embedded::embedded_profile_defaults;
void require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
bool valid_text(std::string_view s, bool empty) {
    if ((!empty && s.empty()) || s.size() > 255) return false;
    for (const unsigned char c : s) if (c < 32 || c == 127) return false;
    return true;
}
std::uint32_t checksum(std::string_view s) {
    std::uint32_t crc = 0xffffffff;
    for (const unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320 & (0U - (crc & 1)));
    }
    return ~crc;
}
std::string read_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    require(size <= max_bytes, "Local profile exceeds size limit");
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(size));
    require(in.good() && in.peek() == std::char_traits<char>::eof(), "Cannot read local profile");
    return text;
}
}

namespace detail {
using Json = dingosdk::Json;
Json parse_json(std::string_view text) {
    return Json::parse(text, {max_bytes, 32, max_records * 16});
}
std::uint64_t unsigned_value(const Json& value, std::uint64_t maximum) {
    require(value.is_number_unsigned(), "Profile value must be a nonnegative integer");
    const auto result = value.get<std::uint64_t>();
    require(result <= maximum, "Profile integer is outside its supported range");
    return result;
}
const Json& object_field(const Json& object, const char* name) {
    const auto& value = object.at(name);
    require(value.is_object(), "Profile section must be a JSON object");
    return value;
}
void validate_object_dropper(const Snapshot& s) {
    if (!s.extensions.contains("object_dropper")) return;
    const auto& section = object_field(s.extensions, "object_dropper");
    if (section.contains("limits")) {
        const auto& limits = object_field(section, "limits");
        if (limits.contains("enabled")) require(limits.at("enabled").is_boolean(), "Invalid building limits enabled flag");
        if (limits.contains("max_objects")) {
            const auto& count = limits.at("max_objects");
            require(count.is_number_integer() && count >= 1 && count <= 1024,
                "Building object limit must be an integer in 1..1024");
        }
        if (limits.contains("radius_metres")) {
            const auto& radius = limits.at("radius_metres");
            require(radius.is_number(), "Building radius must be a number");
            const auto value = radius.get<double>();
            require(std::isfinite(value) && value >= 1 && value <= 5000, "Building radius must be 1..5000 metres");
        }
    }
    if (section.contains("inventory")) {
        const auto& inventory = object_field(section, "inventory");
        require(inventory.size() <= 8192, "Object inventory exceeds limit");
        for (const auto& [key, owned] : inventory.items())
            require(valid_text(key) && owned.is_boolean(), "Invalid object ownership record");
    }
    if (section.contains("catalog")) {
        const auto& catalog = object_field(section, "catalog");
        require(catalog.size() <= 8192, "Object metadata catalog exceeds limit");
        for (const auto& [key, metadata] : catalog.items()) {
            require(valid_text(key) && metadata.is_object(), "Invalid object metadata record");
            for (const auto* field : {"title", "description", "rarity_id"})
                if (metadata.contains(field))
                    require(metadata.at(field).is_string() && valid_text(metadata.at(field).get<std::string>(), true),
                        "Object metadata must be text of at most 255 bytes without control characters");
        }
    }
}
}



void detail::validate_settings(const Snapshot& s) {
    if (s.settings.contains("gameplay")) {
        const auto& values = s.settings.at("gameplay");
        require(values.is_object() && values.size() <= 2048, "Invalid gameplay settings");
        for (const auto& [key, value] : values.items()) {
            require(valid_text(key), "Invalid gameplay setting key");
            // Music selections are ordered identity lists, not native scalar
            // option. The existing settings codec can store its JSON array.
            if (key == "ReSkate.MusicFavoriteSongs" || key == "ReSkate.MusicLikedPlaylists") {
                require(value.is_array(), "Invalid saved music favorites");
                for (const auto& song : value)
                    require(song.is_string() && valid_text(song.string()), "Invalid saved favorite song identity");
                continue;
            }
            require((value.is_number_integer() && (!value.is_number_unsigned() ||
                value.get<std::uint64_t>() <= INT64_MAX)) ||
                (value.is_number_float() && std::isfinite(value.get<double>())) ||
                (value.is_string() && value.string().size() <= 4096 &&
                    value.string().find('\0') == std::string::npos), "Invalid gameplay setting value");
        }
    }
    if (s.settings.contains("native_profile_options")) {
        const auto& groups = s.settings.at("native_profile_options");
        require(groups.is_object(), "Invalid native profile option groups");
        for (const auto& [location, values] : groups.items()) {
            require((location == "1" || location == "2") && values.is_object() && values.size() <= 2048,
                "Invalid native profile option location");
            for (const auto& [key, value] : values.items()) {
                require(valid_text(key), "Invalid native profile option key");
                require(value.is_boolean() || (value.is_number_integer() && (!value.is_number_unsigned() ||
                    value.get<std::uint64_t>() <= INT64_MAX)) ||
                    (value.is_number_float() && std::isfinite(value.get<double>())) ||
                    (value.is_string() && value.string().size() <= 4096 &&
                        value.string().find('\0') == std::string::npos),
                    "Invalid native profile option value");
            }
        }
    }
}
static Json profile_json(const Snapshot& s) {
    require(s.offline_rank_cap > 0 && s.offline_rank_cap <= 10000, "Invalid offline neighborhood rank cap");
    require(s.bool_options.size() + s.play_events.size() + s.quests.size() + s.entitlements.size() +
        s.neighborhood_ranks.size() <= max_records, "Too many local profile records");
    require(s.customization.is_object() && s.settings.is_object() && s.extensions.is_object(),
        "Customization, settings and extensions must be objects");
    detail::validate_settings(s);
    validate_customization(s.customization);
    validate_object_dropper(s);
    validate_challenges(s);
    (void)park_choices(s);
    (void)world_layer_choices(s);
    (void)world_controls(s);
    (void)graphics_controls(s);
    (void)noclip_binding(s);
    (void)forward_velocity_binding(s);
    (void)up_velocity_binding(s);
    (void)news_feed(s);
    (void)rip_score(s);
    (void)location_travel_enabled(s.extensions);
    Json root = s.extensions;
    root["schema_version"] = 1;
    root["revision"] = s.revision;
    root["defaults_version"] = s.onboarding_seed;
    root["neighborhood_rank_cap"] = s.offline_rank_cap;
    root["customization"] = s.customization;
    root["settings"] = s.settings;
    root["options"] = Json::object();
    root["entitlements"] = Json::object();
    auto& progress = root["progress"];
    require(progress.is_null() || progress.is_object(), "Progress must be an object");
    progress["quests"] = Json::object();
    progress["play_events"] = Json::object();
    progress["neighborhood_ranks"] = Json::object();
    for (const auto& [key, value] : s.bool_options) {
        require(valid_text(key), "Invalid option key"); root["options"][key] = value;
    }
    for (const auto& [key, value] : s.entitlements) {
        require(valid_text(key), "Invalid entitlement key"); root["entitlements"][key] = value;
    }
    for (const auto& [key, value] : s.quests) {
        require(valid_text(key) && value >= 0 && value <= 6, "Invalid quest state");
        progress["quests"][key] = value;
    }
    for (const auto& [key, value] : s.neighborhood_ranks) {
        require(std::find(neighborhood_ids.begin(), neighborhood_ids.end(), key) != neighborhood_ids.end() &&
            value <= 10000, "Invalid neighborhood rank");
        progress["neighborhood_ranks"][key] = value;
    }
    for (const auto& [key, event] : s.play_events) {
        require(key == event.id && valid_text(key) && valid_text(event.context, true) && event.count >= 0,
            "Invalid play event");
        progress["play_events"][key] = {{"context", event.context}, {"timestamp", event.timestamp}, {"count", event.count}};
    }
    return root;
}
std::string encode(const Snapshot& s) {
    auto result = profile_json(s).dump(2) + '\n';
    require(result.size() <= max_bytes, "Local profile exceeds size limit");
    return result;
}

static Snapshot decode_document(Json root) {
    require(root.is_object() && unsigned_value(root.at("schema_version"), 1) == 1,
        "Unsupported local profile JSON schema");
    Snapshot s;
    s.revision = unsigned_value(root.at("revision"), (std::numeric_limits<std::uint64_t>::max)());
    s.onboarding_seed = static_cast<std::uint32_t>(unsigned_value(root.at("defaults_version"), UINT32_MAX));
    s.offline_rank_cap = static_cast<std::uint32_t>(unsigned_value(root.at("neighborhood_rank_cap"), 10000));
    const auto parse_bools = [&](const char* name, auto& values) {
        if (!root.contains(name)) return; // an absent section is empty
        for (const auto& [key, value] : object_field(root, name).items()) {
            require(value.is_boolean(), "Profile option/entitlement must be boolean");
            values.emplace(key, value.get<bool>());
        }
    };
    parse_bools("options", s.bool_options); parse_bools("entitlements", s.entitlements);
    const auto& progress = object_field(root, "progress");
    for (const auto& [key, value] : object_field(progress, "quests").items())
        s.quests.emplace(key, static_cast<std::int32_t>(unsigned_value(value, 6)));
    for (const auto& [key, value] : object_field(progress, "neighborhood_ranks").items())
        s.neighborhood_ranks.emplace(key, static_cast<std::uint32_t>(unsigned_value(value, 10000)));
    for (const auto& [key, value] : object_field(progress, "play_events").items()) {
        require(value.is_object() && value.size() == 3 && value.at("context").is_string(), "Invalid play event fields");
        s.play_events.emplace(key, PlayEvent{key, value.at("context").get<std::string>(),
            unsigned_value(value.at("timestamp"), UINT64_MAX),
            static_cast<std::int32_t>(unsigned_value(value.at("count"), INT32_MAX))});
    }
    s.customization = root.contains("customization") ? object_field(root, "customization") : Json::object();
    s.settings = object_field(root, "settings");
    for (const auto* key : {"schema_version", "revision", "defaults_version", "neighborhood_rank_cap",
        "customization", "settings", "options", "entitlements"}) root.erase(key);
    for (const auto* key : {"quests", "neighborhood_ranks", "play_events"}) root["progress"].erase(key);
    if (root["progress"].empty()) root.erase("progress");
    s.extensions = std::move(root);
    (void)profile_json(s);
    return s;
}

Snapshot decode(std::string_view text) { return decode_document(parse_json(text)); }

std::filesystem::path default_path() {
    std::array<wchar_t, 32768> buffer{};
    const auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    require(n && n < buffer.size(), "LOCALAPPDATA is unavailable");
    return std::filesystem::path(buffer.data()) / L"ReSkate" / L"profiles" / L"offline" / L"reskate.sqlite3";
}
std::filesystem::path defaults_path() {
    std::array<wchar_t, 32768> buffer{};
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(count && count < buffer.size(), "Cannot resolve ReSkate defaults path");
    return std::filesystem::path(buffer.data()).parent_path() / L"reskate.defaults.json";
}
// Entitlement ids are game data: every one in the installed content cache is
// owned by default, alongside anything the repository defaults add.
std::string with_cached_entitlements(std::string text) {
    const auto& content = content_cache::catalogs();
    if (!content.available) return text;
    auto document = Json::parse(text);
    auto& entitlements = document["entitlements"];
    for (const auto& id : content.entitlements)
        if (!entitlements.contains(id)) entitlements[id] = true;
    return document.dump();
}
std::string_view embedded_defaults() noexcept {
    return {reinterpret_cast<const char*>(embedded_profile_defaults), sizeof(embedded_profile_defaults)};
}
namespace {
std::atomic<std::uint64_t> store_changes{1};
}
Store::Store(std::filesystem::path path, const std::filesystem::path& defaults) : path_(storage::database_path(std::move(path))),
    database_(std::make_unique<storage::SaveDatabase>(path_, database::profile_schema())) {
    const auto seed = decode(with_cached_entitlements(defaults.empty() || !std::filesystem::exists(defaults) ?
        std::string(embedded_defaults()) : read_file(defaults)));
    if (database_->exists()) {
        value_ = decode_document(database::profile_document(database_->document()));
        database_->backup();
    } else {
        const auto json = storage::json_source_path(path_);
        const auto legacy = path_.parent_path() / L"profile.rsp";
        if (std::filesystem::exists(json)) value_ = decode(read_file(json));
        else {
            require(!std::filesystem::exists(json.wstring() + L".bak"), "JSON profile missing while backup exists; restore before migrating");
            if (std::filesystem::exists(legacy)) value_ = decode_legacy(read_file(legacy));
            else require(!std::filesystem::exists(legacy.wstring() + L".bak"), "Legacy profile backup requires recovery");
        }
    }
    auto next = value_;
    seed_completed_onboarding(next, seed);
    trim_challenge_changes(next);
    // Object Browser categories and travel destinations come from the content
    // cache (object_categories.h, location_travel.h); saves keep only settings.
    if (next.extensions.contains("object_dropper") && next.extensions.at("object_dropper").is_object())
        next.extensions.at("object_dropper").erase("categories");
    if (next.extensions.contains("location_travel") && next.extensions.at("location_travel").is_object())
        for (const auto* key : {"destinations", "access_points"}) next.extensions.at("location_travel").erase(key);
    if (!database_->exists() || next != value_) commit(std::move(next));
    note_change();
}
Store::~Store() = default;
std::uint64_t Store::changes() noexcept { return store_changes.load(std::memory_order_acquire); }
void Store::note_change() noexcept { store_changes.fetch_add(1, std::memory_order_release); }
Snapshot Store::snapshot() const { std::lock_guard lock(mutex_); return value_; }
std::shared_ptr<const Snapshot> Store::shared_snapshot() const {
    std::lock_guard lock(mutex_);
    if (!snapshot_cache_ || snapshot_cache_->revision != value_.revision) snapshot_cache_ = std::make_shared<const Snapshot>(value_);
    return snapshot_cache_;
}
std::string Store::export_json() const { std::lock_guard lock(mutex_); return encode(value_); }
void Store::import_json(std::string_view text) {
    auto next = decode(text);
    std::lock_guard lock(mutex_);
    next.revision = value_.revision;
    if (next != value_) commit(std::move(next));
}
std::uint64_t Store::revision() const { std::lock_guard lock(mutex_); return value_.revision; }
std::shared_ptr<const MissionState> Store::mission_state() const {
    std::lock_guard lock(mutex_);
    if (!mission_cache_ || mission_cache_->revision != value_.revision) {
        auto result = std::make_shared<MissionState>();
        result->revision = value_.revision;
        result->play_events = value_.play_events;
        result->quests = value_.quests;
        mission_cache_ = std::move(result);
    }
    return mission_cache_;
}
NeighborhoodState Store::neighborhood_state(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const auto enabled = [&](std::string_view key) {
        const auto it = value_.bool_options.find(key);
        return it != value_.bool_options.end() && it->second;
    };
    NeighborhoodState result{enabled(unlock_neighborhoods_option), enabled(max_neighborhood_ranks_option), {}};
    const auto rank = value_.neighborhood_ranks.find(id);
    if (rank != value_.neighborhood_ranks.end()) result.rank = rank->second;
    return result;
}
std::uint32_t Store::offline_rank_cap() const {
    std::lock_guard lock(mutex_); return value_.offline_rank_cap;
}
std::optional<RipScore> Store::saved_rip_score() const {
    std::lock_guard lock(mutex_); return profile::rip_score(value_);
}
std::vector<std::string> Store::completed_challenge_criteria(std::string_view id) const {
    std::lock_guard lock(mutex_); return profile::challenge_completed_criteria(value_, id);
}
std::optional<bool> Store::bool_option(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto it = value_.bool_options.find(key);
    return it == value_.bool_options.end() ? std::nullopt : std::optional<bool>(it->second);
}
std::optional<Json> Store::user_value(std::string_view key) const {
    std::lock_guard lock(mutex_);
    // Share Boolean storage with the existing profile-option expression hooks.
    if (const auto it = value_.bool_options.find(key); it != value_.bool_options.end()) return Json(it->second);
    if (!value_.settings.contains("gameplay")) return {};
    const auto& values = value_.settings.at("gameplay");
    const auto it = values.find(std::string(key));
    return it == values.end() ? std::nullopt : std::optional<Json>(*it);
}
void Store::set_user_value(std::string_view key, const Json& value) {
    if (value.is_boolean()) { set_bool_option(key, value.get<bool>()); return; }
    std::lock_guard lock(mutex_);
    require(valid_text(key) && !value_.bool_options.contains(key), "Invalid gameplay setting key or type");
    Update update(*this);
    update.json(value_.settings, {"gameplay", key}) = value;
    update.commit();

}
void Store::set_user_values(const std::vector<std::pair<std::string, Json>>& values) {
    if (values.empty()) return;
    std::lock_guard lock(mutex_);
    Update update(*this);
    for (const auto& [key, value] : values) {
        require(valid_text(key), "Invalid gameplay setting key");
        if (value.is_boolean()) {
            update.record(value_.bool_options, key, "player_settings") = value.get<bool>();
            continue;
        }
        require(!value_.bool_options.contains(key), "Invalid gameplay setting key or type");
        update.json(value_.settings, {"gameplay", key}) = value;
    }
    update.commit();
}
std::optional<std::int32_t> Store::quest_state(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto it = value_.quests.find(key);
    return it == value_.quests.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
}
std::optional<Json> Store::native_profile_option(unsigned location, std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto section = value_.settings.find("native_profile_options");
    if (section == value_.settings.end()) return {};
    const auto group = section->find(std::to_string(location));
    if (group == section->end()) return {};
    const auto option = group->find(std::string(key));
    return option == group->end() ? std::nullopt : std::optional<Json>(*option);
}
void Store::set_native_profile_option(unsigned location, std::string_view key, const Json& value) {
    std::lock_guard lock(mutex_);
    require((location == 1 || location == 2) && valid_text(key), "Invalid native profile option location or key");
    Update update(*this);
    update.json(value_.settings, {"native_profile_options", std::to_string(location), key}) = value;
    update.commit();

}
PlayEvent Store::record_play_event(std::string_view id) {
    std::lock_guard lock(mutex_);
    require(valid_text(id), "Invalid event identifier");
    Update update(*this);
    auto& event = update.record(value_.play_events, id, "play_events");
    if (event.id.empty()) event.id = id;
    require(event.count < INT32_MAX, "Play-event count overflow");
    ++event.count;
    const auto result = event;
    update.commit();
    return result;

}
std::optional<bool> Store::entitlement(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto it = value_.entitlements.find(key);
    return it == value_.entitlements.end() ? std::nullopt : std::optional<bool>(it->second);
}
void Store::set_neighborhood_rank(std::string_view key, std::uint32_t value) {
    std::lock_guard lock(mutex_);
    require(std::find(neighborhood_ids.begin(), neighborhood_ids.end(), key) != neighborhood_ids.end() && value <= 10000, "Invalid neighborhood rank");
    Update update(*this);
    update.record(value_.neighborhood_ranks, key, "neighborhood_ranks") = value;
    update.commit();

}
void Store::set_bool_option(std::string_view key, bool value) {
    std::lock_guard lock(mutex_);
    require(valid_text(key), "Invalid option identifier");
    Update update(*this);
    update.record(value_.bool_options, key, "player_settings") = value;
    update.commit();

}
void Store::set_quest_state(std::string_view id, std::int32_t value) {
    std::lock_guard lock(mutex_);
    require(valid_text(id) && value >= 0 && value <= 6, "Invalid quest state");
    Update update(*this);
    update.record(value_.quests, id, "quests") = value;
    update.commit();

}
void Store::set_onboarding_completed(std::string_view id, bool completed) {
    std::lock_guard lock(mutex_);
    require(std::find(onboarding_ids.begin(), onboarding_ids.end(), id) != onboarding_ids.end(), "Unknown onboarding mission");
    Update update(*this);
    for (const auto* suffix : {":start", ":complete"}) {
        const auto key = std::string(id) + suffix;
        auto& event = update.record(value_.play_events, key, "play_events");
        if (event.id.empty()) event.id = key;
        event.count = completed ? (std::max)(1, event.count) : 0;
    }
    update.commit();

}
void Store::commit(Snapshot next) {
    require(value_.revision != (std::numeric_limits<std::uint64_t>::max)(), "Profile revision overflow");
    next.revision = value_.revision + 1;
    auto tables = database::profile_tables(profile_json(next), database_->exists() ? &database_->document() : nullptr, &value_);
    if (database_->exists()) database_->commit(std::move(tables));
    else database_->initialize(std::move(tables));
    value_ = std::move(next);
    note_change();
}

}
