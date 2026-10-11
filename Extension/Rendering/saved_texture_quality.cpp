#include "saved_texture_quality.h"
#include "Launcher/game_settings.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <variant>

namespace dingosdk {
namespace {
namespace fs = std::filesystem;
namespace gs = launcher_game_settings;

// The newest write time of the save containers' DATA files (data\<account>\<title>\<container>),
// or 0 when there are none. Cheap: a few directory entries, no file is opened.
std::int64_t newest_save(const fs::path& root) {
    std::int64_t newest = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
        if (it.depth() != 2 || !it->is_directory(error)) continue;
        std::error_code missing;
        const auto written = fs::last_write_time(it->path() / L"DATA", missing);
        if (!missing) newest = std::max<std::int64_t>(newest, written.time_since_epoch().count());
    }
    return newest;
}
}

SavedTextureQuality saved_texture_quality() {
    static SavedTextureQuality saved;
    static std::int64_t read_at = 0;
    try {
        const auto root = gs::save_root();
        if (root.empty()) return saved;
        const auto newest = newest_save(root);
        if (!newest || newest == read_at) return saved;
        // A save caught mid-write throws; it is read again on the next call.
        const auto values = gs::load(root).values;
        texture_tier::Tier tier = texture_tier::unknown;
        if (const auto options = values.find(gs::options_key);
            options != values.end() && std::holds_alternative<std::string>(options->second))
            for (const auto& option : gs::parse_options(std::get<std::string>(options->second)))
                if (option.name == "Texture Quality") tier = texture_tier::from_choice_key(option.value);
        read_at = newest;
        saved.tier = tier;
        ++saved.generation;
    } catch (...) {}
    return saved;
}
}
