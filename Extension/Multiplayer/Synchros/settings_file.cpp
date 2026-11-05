#include "settings_file.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace dingosdk::multiplayer::synchros {
namespace {

bool valid_symbols(const std::string& code, CodeKind kind) {
    const auto read = read_code(code, kind);
    return read && *read == code;
}

} // namespace

std::string settings_json(const Settings& s) {
    auto root = Json::object();
    root["share"] = s.share;
    root["show"] = s.show;
    root["reload_on_arrival"] = s.reload_on_arrival;
    root["own_code"] = s.own_code;
    auto paired = Json::array();
    for (const auto id : s.paired) paired.push_back(std::to_string(id));
    root["paired"] = std::move(paired);
    auto entered = Json::array();
    for (const auto& code : s.entered) entered.push_back(code);
    root["codes"] = std::move(entered);
    auto groups = Json::array();
    for (const auto& g : s.groups) {
        auto group = Json::object();
        group["name"] = g.name;
        group["code"] = g.code;
        group["created"] = g.created;
        groups.push_back(std::move(group));
    }
    root["groups"] = std::move(groups);
    auto blocked = Json::array();
    for (const auto id : s.blocked) blocked.push_back(std::to_string(id));
    root["blocked"] = std::move(blocked);
    return root.dump(2);
}

Settings settings_from_json(std::string_view text) {
    Settings s;
    Json root;
    try {
        root = Json::parse(text);
    } catch (...) {
        return {};
    }
    if (!root.is_object()) return {};
    const auto flag = [&](const char* key) {
        return root.contains(key) && root.at(key).is_boolean() && root.at(key).get<bool>();
    };
    const auto ids = [&](const char* key, std::set<std::uint64_t>& out) {
        if (!root.contains(key) || !root.at(key).is_array()) return;
        for (const auto& v : root.at(key)) {
            if (!v.is_string() || out.size() >= 4096) continue;
            try {
                const auto id = std::stoull(v.string());
                if (id) out.insert(id);
            } catch (...) {}
        }
    };
    s.share = flag("share");
    s.show = flag("show");
    s.reload_on_arrival = !root.contains("reload_on_arrival") || flag("reload_on_arrival");
    if (root.contains("own_code") && root.at("own_code").is_string() && valid_symbols(root.at("own_code").string(), CodeKind::personal))
        s.own_code = root.at("own_code").string();
    ids("paired", s.paired);
    ids("blocked", s.blocked);
    if (root.contains("codes") && root.at("codes").is_array())
        for (const auto& v : root.at("codes"))
            if (v.is_string() && s.entered.size() < max_entered_codes && valid_symbols(v.string(), CodeKind::personal) &&
                std::find(s.entered.begin(), s.entered.end(), v.string()) == s.entered.end())
                s.entered.push_back(v.string());
    if (root.contains("groups") && root.at("groups").is_array())
        for (const auto& v : root.at("groups")) {
            if (!v.is_object() || s.groups.size() >= max_groups) continue;
            Group g;
            if (v.contains("code") && v.at("code").is_string()) g.code = v.at("code").string();
            if (!valid_symbols(g.code, CodeKind::group)) continue;
            if (v.contains("name") && v.at("name").is_string()) g.name = v.at("name").string().substr(0, max_group_name);
            g.created = v.contains("created") && v.at("created").is_boolean() && v.at("created").get<bool>();
            s.groups.push_back(std::move(g));
        }
    return s;
}

Settings load_settings(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > (1u << 20)) return {};
    return settings_from_json(text);
}

bool save_settings(const std::filesystem::path& file, const Settings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto temporary = file;
    temporary += ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << settings_json(settings);
        if (!out) return false;
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) std::filesystem::remove(temporary, ec);
    return !ec;
}

} // namespace dingosdk::multiplayer::synchros
