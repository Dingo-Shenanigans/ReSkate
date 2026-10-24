#include "content_catalogs.h"
#include "content_cache.h"
#include "Engine/Resource/protobuf_wire.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace dingosdk::content_cache {
namespace {
using protobuf::Field;
using protobuf::Wire;
using Fields = std::vector<Field>;

// Catalogue strings are UTF-8 text without control characters.
std::optional<std::string> text(const Field* field) {
    if (!field) return std::nullopt;
    const auto value = protobuf::text(field->bytes);
    if (std::any_of(value.begin(), value.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte < 0x20 ? c != '\t' && c != '\n' && c != '\r' : byte == 0x7f;
        })) return std::nullopt;
    return std::string(value);
}
std::optional<std::string> text(const Fields& fields, std::uint32_t number) {
    return text(protobuf::first(fields, number, Wire::bytes));
}
std::optional<Fields> message(const Fields& fields, std::uint32_t number) {
    const auto* field = protobuf::first(fields, number, Wire::bytes);
    return field ? protobuf::parse(field->bytes) : std::nullopt;
}
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

// Owned items: the presentation message (6) names the owned asset (6.10), the
// title (6.2) and description (6.3); the rarity is field 23. First record wins.
// Field 10 set marks an item owned from the start; field 11 names the
// neighbourhood rank that unlocks it, which some items carry with no rank
// record listing them as a reward.
void add_item(Catalogs& out, const Fields& record) {
    const auto presentation = message(record, 6);
    if (!presentation) return;
    const auto asset = text(*presentation, 10);
    if (!asset || lower(asset->substr(0, 4)) != "own_") return;
    const auto key = lower(*asset);
    if (out.items.contains(key)) return;
    if (const auto* flag = protobuf::first(record, 10, Wire::varint); flag && flag->integer == 1)
        out.open_items.insert(key);
    // E.g. Pole Jam S/L and the Stop sign: rank unlocks that add_source never
    // sees, because no neighbourhood_rank record grants them.
    if (text(record, 11).value_or("").starts_with("neighbourhood_rank"))
        out.open_items.insert(key);
    // Field 24 tags brand collabs (collab_xgames, collab_nike, ...).
    if (text(record, 24).value_or("").starts_with("collab_"))
        out.collab_items.insert(key);
    Json entry = Json::object();
    if (const auto description = text(*presentation, 3)) entry["description"] = *description;
    if (const auto rarity = text(record, 23)) entry["rarity_id"] = *rarity;
    if (const auto title = text(*presentation, 2)) entry["title"] = *title;
    if (const auto group = text(*presentation, 5)) entry["group"] = *group;
    if (const auto type = text(*presentation, 6)) entry["object_type"] = *type;
    out.items[key] = std::move(entry);
}

// Music playlists: field 1 names the playlist, repeated field 2 lists tracks
// ("Artist - Title"), and field 10 is a message carrying the display name and
// artwork. Owned-item records put a varint in field 10, so requiring the byte
// wire separates the two. First record for an id wins.
void add_music_playlist(Catalogs& out, const std::string& id, const Fields& record) {
    const auto content = message(record, 10);
    if (!content) return;
    const auto name = text(*content, 10);
    if (!name) return;
    std::vector<std::string> tracks;
    for (const auto& field : record) {
        if (field.number != 2 || field.wire != Wire::bytes) continue;
        const auto value = text(&field);
        if (!value || value->find(" - ") == std::string::npos) continue;
        tracks.push_back(*value);
    }
    if (tracks.size() < 2) return;
    Catalogs::MusicPlaylistEntry entry;
    entry.name = *name;
    entry.artwork = text(*content, 11).value_or("");
    entry.tracks = std::move(tracks);
    out.music_playlists.try_emplace(id, std::move(entry));
}

// Songs: field 1 is "Artist - Title" and field 10 a message with the artist (10), title (11) and
// cdn:/ cover art (12). First record for an id wins.
void add_music_song(Catalogs& out, const std::string& id, const Fields& record) {
    if (id.find(" - ") == std::string::npos) return;
    const auto content = message(record, 10);
    if (!content || !text(*content, 10) || !text(*content, 11)) return;
    if (const auto artwork = text(*content, 12); artwork && artwork->starts_with("cdn:/"))
        out.music_song_artwork.try_emplace(id, *artwork);
}

// Grant lists: repeated `number` entries, each {1: kind, 2: id}.
struct Grant { std::string kind, id; };
std::vector<Grant> grants(const Fields& fields, std::uint32_t number) {
    std::vector<Grant> out;
    for (const auto& field : fields) {
        if (field.number != number || field.wire != Wire::bytes) continue;
        const auto list = protobuf::parse(field.bytes);
        if (!list) continue;
        for (const auto& entry : *list) {
            if (entry.number != 1 || entry.wire != Wire::bytes) continue;
            const auto grant = protobuf::parse(entry.bytes);
            if (!grant) continue;
            if (auto kind = text(*grant, 1), id = text(*grant, 2); kind && id)
                out.push_back({std::move(*kind), std::move(*id)});
        }
    }
    return out;
}
bool free_offer(const Fields& offer) {
    const auto price = message(offer, 4);
    const auto costs = price ? grants(*price, 2) : std::vector<Grant>{};
    if (costs.empty() || !std::all_of(costs.begin(), costs.end(), [](const Grant& cost) {
            return cost.kind == "res" && (cost.id == "influence" || cost.id == "skatepass_currency");
        })) return false;
    const auto prerequisites = grants(offer, 8);
    return std::none_of(prerequisites.begin(), prerequisites.end(), [](const Grant& prerequisite) {
        return lower(prerequisite.id).find("premium") != std::string::npos;
    });
}
// Every own-create grant in any of the record's grant lists. Deliberately broad:
// it does not tell free from paid, so an item named by any offer, store bundle,
// pack or reward counts as granted and keeps the store's say over it (reserved()).
// A bytes field that is not a grant list simply parses to no grants.
void add_granted(std::set<std::string>& granted, const Fields& record) {
    std::set<std::uint32_t> numbers;
    for (const auto& field : record)
        if (field.wire == Wire::bytes) numbers.insert(field.number);
    for (const auto number : numbers)
        for (const auto& grant : grants(record, number))
            if (grant.kind == "own-create" && lower(grant.id.substr(0, 4)) == "own_") granted.insert(lower(grant.id));
}
void add_source(std::set<std::string>& open, const std::string& id, const Fields& record) {
    const auto open_grants = [&](std::uint32_t number) {
        for (const auto& grant : grants(record, number))
            if (grant.kind == "own-create" && lower(grant.id.substr(0, 4)) == "own_") open.insert(lower(grant.id));
    };
    if (id.ends_with("Price")) {
        if (free_offer(record)) open_grants(11);
        return;
    }
    open_grants(8);
    const auto kind = text(record, 3).value_or("");
    if (kind == "progression-item") open_grants(7);
    if (kind.find("-event-") != std::string::npos) open_grants(4);
    if (text(record, 4).value_or("").starts_with("neighbourhood_rank")) open_grants(20);
}
std::vector<std::string> texts(const Fields& fields, std::uint32_t number) {
    std::vector<std::string> result;
    for (const auto& field : fields)
        if (field.number == number && field.wire == Wire::bytes)
            if (auto value = text(&field)) result.push_back(std::move(*value));
    return result;
}
std::uint32_t priority(const Fields& fields) {
    const auto* field = protobuf::first(fields, 2, Wire::varint);
    return field ? static_cast<std::uint32_t>(field->integer) : 0;
}

// Category service records: id (1), priority (2), sub-categories (5, each with
// id 1, priority 2 and presentation 6 holding the title in 1), tags (6) and a
// presentation (7) with the title (1) and icon (3). Build-kit categories carry
// the "buildkit" tag; the Object Browser lists the ones tagged "qdbuildkit".
void add_object_category(Catalogs& out, const std::string& id, const Fields& record) {
    const auto tags = texts(record, 6);
    if (std::find(tags.begin(), tags.end(), "buildkit") == tags.end()) return;
    const auto presentation = message(record, 7);
    if (!presentation) return;
    ObjectCategory category{id, text(*presentation, 1).value_or(id), text(*presentation, 3).value_or(""),
        priority(record), std::find(tags.begin(), tags.end(), "qdbuildkit") != tags.end(), {}};
    for (const auto& field : record) {
        if (field.number != 5 || field.wire != Wire::bytes) continue;
        const auto group = protobuf::parse(field.bytes);
        if (!group) return;
        const auto group_id = text(*group, 1);
        if (!group_id) return;
        const auto title = message(*group, 6);
        category.groups.push_back({*group_id, (title ? text(*title, 1) : std::nullopt).value_or(*group_id),
            priority(*group)});
    }
    for (const auto& known : out.object_categories)
        if (known.id == id) return;
    out.object_categories.push_back(std::move(category));
}

// Challenges are the records with goals (21). Type is field 2, neighbourhood 7,
// the title key 30.10 (30.11 is the type's name, 30.12 the short description). Each goal has its id (1), order (2), optional flag (5)
// and text keys in 10: title 10, feed 11, short description 12, description 13.
void add_challenge(Catalogs& out, const std::string& id, const Fields& record) {
    struct Goal { std::uint64_t order; Json value; };
    std::vector<Goal> goals;
    for (const auto& field : record) {
        if (field.number != 21 || field.wire != Wire::bytes) continue;
        const auto goal = protobuf::parse(field.bytes);
        if (!goal) return;
        const auto goal_id = text(*goal, 1);
        if (!goal_id) return;
        const auto* order = protobuf::first(*goal, 2, Wire::varint);
        const auto* optional = protobuf::first(*goal, 5, Wire::varint);
        Json value{{"id", *goal_id}, {"optional", optional && optional->integer != 0}};
        if (const auto keys = message(*goal, 10)) {
            for (const auto& [number, name] : {std::pair{10u, "title_key"}, {11u, "feed_key"},
                     {12u, "short_description_key"}, {13u, "description_key"}})
                if (const auto key = text(*keys, number)) value[name] = *key;
        }
        goals.push_back({order ? order->integer : 0, std::move(value)});
    }
    const auto type = text(record, 2);
    if (goals.empty() || !type) return;
    std::stable_sort(goals.begin(), goals.end(), [](const Goal& a, const Goal& b) { return a.order < b.order; });
    Json entry{{"type", *type}, {"available", true},
        {"asset", "activities/challenges/prefabs/" + lower(*type) + "/" + lower(id) + "_ecsprefab"}};
    auto list = Json::array();
    for (auto& goal : goals) list.push_back(std::move(goal.value));
    entry["goals"] = std::move(list);
    if (const auto presentation = message(record, 30)) {
        if (const auto title = text(*presentation, 10)) entry["title_key"] = *title;
        if (const auto description = text(*presentation, 12)) entry["description_key"] = *description;
    }
    if (const auto neighborhood = text(record, 7)) entry["neighborhood"] = *neighborhood;
    out.challenges[id] = std::move(entry);
}

// Travel: locations carry the level (3) and a presentation (30) with the title
// (2), description (3), image (5), medium (6) and white/black icons (7, 8).
// Access points list their destinations in field 2.
void add_travel(Catalogs& out, const std::string& id, const Fields& record) {
    if (id.starts_with("accesspoint_")) {
        for (const auto& known : out.travel_access_points)
            if (known.first == id) return;
        auto targets = texts(record, 2);
        if (!targets.empty()) out.travel_access_points.emplace_back(id, std::move(targets));
        return;
    }
    if (!id.starts_with("location_")) return;
    const auto level = text(record, 3);
    const auto presentation = message(record, 30);
    if (!level || !presentation) return;
    for (const auto& known : out.travel_locations)
        if (known.id == id) return;
    const auto field = [&](std::uint32_t number) { return text(*presentation, number).value_or(""); };
    out.travel_locations.push_back({id, *level, field(2), field(3), field(6), field(7), field(8), field(5)});
}

std::vector<unsigned char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

Catalogs read_catalogs(const std::filesystem::path& folder) {
    Catalogs out;
    std::vector<std::filesystem::path> bodies;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.is_regular_file() && entry.path().extension() == L".cache") bodies.push_back(entry.path());
    std::sort(bodies.begin(), bodies.end());
    std::set<std::string> entitlements;
    for (const auto& path : bodies) {
        const auto bytes = read_file(path);
        // Image bodies (DDS, PNG) and anything else that is not a framed
        // record stream is skipped as a whole.
        const auto records = protobuf::frames(bytes);
        if (!records) continue;
        for (const auto record : *records) {
            const auto fields = protobuf::parse(record);
            if (!fields) continue;
            const auto id = text(*fields, 1);
            if (!id) continue;
            if (text(*fields, 3) == "entitlement") entitlements.insert(*id);
            add_item(out, *fields);
            add_music_playlist(out, *id, *fields);
            add_music_song(out, *id, *fields);
            add_challenge(out, *id, *fields);
            add_object_category(out, *id, *fields);
            add_travel(out, *id, *fields);
            add_source(out.open_items, *id, *fields);
            add_granted(out.granted_items, *fields);
        }
    }
    out.entitlements.assign(entitlements.begin(), entitlements.end());
    out.available = true;
    return out;
}

bool Catalogs::reserved(const std::string& key) const {
    if (!items.contains(key) || open_items.contains(key)) return false;
    // Build-kit objects nothing offers, sells or grants are taken as everyday
    // park pieces; brand collabs stay with the store. Many basic objects (A
    // frames, quarter pipes, bowl pieces, Pole Jam M) have no source record in
    // the cache, so without this the Object Browser and park editor hid them.
    // Cosmetics are not covered: unsourced ones include past store and pass items.
    if (key.starts_with("own_bk") && !granted_items.contains(key) && !collab_items.contains(key)) return false;
    return !key.starts_with("dev_") && key.find("_dev_") == std::string::npos && !key.ends_with("_dev");
}

namespace {
// reskate-objects.json text: at most 255 bytes, no control characters. Anything else is absent.
std::optional<std::string> mod_text(const Json& entry, std::string_view field) {
    if (!entry.contains(field) || !entry.at(field).is_string()) return std::nullopt;
    const auto& value = entry.at(field).string();
    if (value.empty() || value.size() > 255 || std::any_of(value.begin(), value.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte < 0x20 || byte == 0x7f;
        })) return std::nullopt;
    return value;
}
// The entry's priority, or `after` (one past the highest already there) when it gives none.
std::uint32_t mod_priority(const Json& entry, std::uint32_t after) {
    if (entry.contains("priority") && entry.at("priority").is_number_integer()) {
        const auto value = entry.at("priority").get<std::int64_t>();
        if (value >= 0 && value <= 0xffffffffLL) return static_cast<std::uint32_t>(value);
    }
    return after;
}
const Json& mod_list(const Json& root, std::string_view name) {
    static const Json none = Json::array();
    if (!root.contains(name)) return none;
    if (!root.at(name).is_array()) throw std::runtime_error(std::string(name) + " is not a list");
    return root.at(name);
}
ObjectCategory* find_category(Catalogs& out, std::string_view id) {
    for (auto& category : out.object_categories)
        if (category.id == id) return &category;
    return nullptr;
}
bool family_exists(const Catalogs& out, std::string_view id) {
    return std::ranges::any_of(out.object_categories, [&](const ObjectCategory& category) {
        return std::ranges::any_of(category.groups, [&](const ObjectGroup& group) { return group.id == id; });
    });
}
}

std::vector<std::string> add_mod_objects(Catalogs& out, const std::string& json) {
    const auto root = Json::parse(json);
    if (!root.is_object() || !root.contains("schema") || !root.at("schema").is_number_integer() ||
        root.at("schema").get<int>() != 1)
        throw std::runtime_error("expected schema 1");
    // Checked before anything is added, so a file that is not this shape adds nothing.
    const auto& categories = mod_list(root, "categories");
    const auto& families = mod_list(root, "families");
    const auto& objects = mod_list(root, "objects");
    std::vector<std::string> refused;
    for (std::size_t i = 0; i < categories.size() && i < 64; ++i) {
        const auto& entry = categories.at(i);
        const auto id = mod_text(entry, "id");
        if (!id) { refused.push_back("a category without an id was left out"); continue; }
        if (find_category(out, *id)) { refused.push_back("category " + *id + " was left out: the id is taken"); continue; }
        std::uint32_t last{};
        for (const auto& category : out.object_categories) last = std::max(last, category.priority);
        // In the Object Browser, as the game's categories tagged "qdbuildkit" are. No icon: the
        // browser draws its first tile's thumbnail.
        out.object_categories.push_back({*id, mod_text(entry, "title").value_or(*id), {},
            mod_priority(entry, last + 1), true, {}});
    }
    for (std::size_t i = 0; i < families.size() && i < 1024; ++i) {
        const auto& entry = families.at(i);
        const auto id = mod_text(entry, "id");
        const auto category_id = mod_text(entry, "category");
        if (!id || !category_id) { refused.push_back("a family without an id and a category was left out"); continue; }
        if (family_exists(out, *id)) { refused.push_back("family " + *id + " was left out: the id is taken"); continue; }
        auto* category = find_category(out, *category_id);
        if (!category) {
            refused.push_back("family " + *id + " was left out: there is no category " + *category_id);
            continue;
        }
        std::uint32_t last{};
        for (const auto& group : category->groups) last = std::max(last, group.priority);
        category->groups.push_back({*id, mod_text(entry, "title").value_or(*id), mod_priority(entry, last + 1)});
    }
    for (std::size_t i = 0; i < objects.size() && i < 8192; ++i) {
        const auto& entry = objects.at(i);
        const auto key = mod_text(entry, "key");
        if (!key || lower(key->substr(0, 4)) != "own_") {
            refused.push_back("an object without an own_ key was left out");
            continue;
        }
        const auto id = lower(*key);
        // The game's own items keep their names and families, and the first mod to name an object wins.
        if (out.items.contains(id)) { refused.push_back("object " + id + " was left out: it is the game's"); continue; }
        if (out.mod_objects.contains(id)) { refused.push_back("object " + id + " was left out: it is already named"); continue; }
        Json object = Json::object();
        if (const auto family = mod_text(entry, "family")) {
            if (!family_exists(out, *family)) {
                refused.push_back("object " + id + " was left out: there is no family " + *family);
                continue;
            }
            object["group"] = *family;
            // What the game's own Quick Drop records give as their BuildKitObjectType.
            object["object_type"] = std::string("QuickDrop");
        }
        if (const auto title = mod_text(entry, "title")) object["title"] = *title;
        if (const auto description = mod_text(entry, "description")) object["description"] = *description;
        if (!object.size()) { refused.push_back("object " + id + " was left out: it has no title or family"); continue; }
        out.mod_objects[id] = std::move(object);
    }
    return refused;
}

namespace {
Catalogs& process_catalogs() {
    static Catalogs value = [] {
        try {
            if (installed()) return read_catalogs(directory());
        } catch (...) {}
        return Catalogs{};
    }();
    return value;
}
}

const Catalogs& catalogs() { return process_catalogs(); }

ModObjectsReport install_mod_objects(const std::vector<std::pair<std::string, std::filesystem::path>>& files) {
    ModObjectsReport report;
    auto& out = process_catalogs();
    for (const auto& [mod, path] : files) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        try {
            if (std::filesystem::file_size(path) > JsonLimits{}.bytes) throw std::runtime_error("it is too large");
            const auto bytes = read_file(path);
            const auto before = out.mod_objects.size();
            for (auto& note : add_mod_objects(out, std::string(bytes.begin(), bytes.end())))
                report.notes.push_back(mod + ": " + note);
            report.objects += out.mod_objects.size() - before;
            ++report.mods;
        } catch (const std::exception& failure) {
            report.notes.push_back(mod + ": " + mod_objects_file + " was skipped: " + failure.what());
        }
    }
    return report;
}
}
