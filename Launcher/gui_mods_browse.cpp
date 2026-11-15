#include "gui_internal.h"
#include "gui_renderer.h"

#include "Engine/Vfs/builtin_packs.h"

#include "mod_manager.h"
#include "updater.h"

#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Debug/trace.h"

#include <cstdio>
#include <cctype>
#include <format>
#include <set>

// The Mods panel's BROWSE page: packages from Thunderstore, their icons, and
// downloading and installing them on the panel's worker.
namespace dingosdk::launcher_gui::detail {
namespace {

namespace ts = thunderstore;

constexpr int listing_timeout_ms = 20000;
constexpr int download_timeout_ms = 30000;
constexpr std::size_t max_icons = 128;          // loaded icon textures, of the renderer's 160 slots
constexpr std::uint64_t max_icon_bytes = 6 * 1024 * 1024;
// Marks an Icons key that is an icon.png on disk rather than a URL.
constexpr std::string_view local_icon_prefix = "file:";

void log(logging::Level level, const std::string& message) {
    logging::write(level, logging::Channel::launcher, message);
}

std::string fetch(const std::wstring& url, std::uint64_t limit, const std::atomic<bool>& stop) {
    std::string body;
    launcher_update::http_stream(url, limit, listing_timeout_ms, [&](const char* data, DWORD size) {
        if (stop) throw std::runtime_error("stopped");
        body.append(data, size);
    });
    return body;
}

// The listing index and its chunks (what r2modman reads), else the plain listing.
std::vector<ts::Package> fetch_listing(const std::string& community, const std::atomic<bool>& stop) {
    try {
        const auto index = ts::parse_index(ts::gunzip(fetch(ts::listing_index_url(community), 1 << 20, stop), 4 << 20));
        std::vector<ts::Package> packages;
        for (const auto& url : index) {
            auto chunk = ts::parse_listing(ts::gunzip(fetch(url, 64ull << 20, stop), 256ull << 20));
            std::move(chunk.begin(), chunk.end(), std::back_inserter(packages));
        }
        return packages;
    } catch (const std::exception& failure) {
        if (stop) throw;
        log(logging::Level::warning, std::string("Thunderstore listing index unavailable (") + failure.what() +
            "); reading the plain listing");
    }
    return ts::parse_listing(ts::gunzip(fetch(ts::listing_url(community), 256ull << 20, stop), 256ull << 20));
}

fs::path icon_cache() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"thunderstore" / L"icons";
}

// Icon URLs end in Namespace-Name-1.2.3.png, so a file name keyed on that never goes stale.
std::wstring icon_file(std::string_view url) {
    const auto slash = url.rfind('/');
    std::string name(url.substr(slash == std::string_view::npos ? 0 : slash + 1));
    for (auto& ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
            ch = '_';
    if (name.empty() || name.front() == '.') name.insert(name.begin(), 'i');
    return wide(name);
}

std::vector<unsigned char> read_bytes(const fs::path& path) {
    std::vector<unsigned char> bytes;
    FILE* file{};
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return bytes;
    std::array<unsigned char, 65536> buffer{};
    for (std::size_t read; (read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0;)
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    std::fclose(file);
    return bytes;
}

void icon_worker(Icons& icons) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const auto cache = icon_cache();
    std::atomic<bool> never{};
    for (;;) {
        std::string url;
        {
            std::unique_lock lock(icons.mutex);
            icons.wake.wait(lock, [&] { return icons.stop || !icons.queue.empty(); });
            if (icons.stop) break;
            url = std::move(icons.queue.front());
            icons.queue.pop_front();
        }
        Icons::Decoded decoded{url};
        try {
            const bool local = url.starts_with(local_icon_prefix);
            const auto cached = local ? fs::path(wide(std::string_view(url).substr(local_icon_prefix.size())))
                              : cache.empty() ? fs::path() : cache / icon_file(url);
            std::error_code size_error;
            auto bytes = cached.empty() || (local && fs::file_size(cached, size_error) > max_icon_bytes)
                ? std::vector<unsigned char>() : read_bytes(cached);
            if (bytes.empty() && !local) {
                const auto body = fetch(wide(url), max_icon_bytes, never);
                bytes.assign(body.begin(), body.end());
                if (!cached.empty()) {
                    std::error_code error;
                    fs::create_directories(cache, error);
                    FILE* file{};
                    if (!_wfopen_s(&file, cached.c_str(), L"wb") && file) {
                        std::fwrite(bytes.data(), 1, bytes.size(), file);
                        std::fclose(file);
                    }
                }
            }
            UINT width{}, height{};
            if (!decode_image(bytes, ImVec2(S(96), S(96)), decoded.pixels, width, height)) decoded.pixels.clear();
            decoded.width = width;
            decoded.height = height;
        } catch (const std::exception&) {
            decoded.pixels.clear();
        }
        std::lock_guard lock(icons.mutex);
        icons.done.push_back(std::move(decoded));
    }
    if (com) CoUninitialize();
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::string count_text(std::uint64_t value) {
    if (value >= 1'000'000) return std::format("{:.1f}M", static_cast<double>(value) / 1e6);
    if (value >= 10'000) return std::format("{}k", value / 1000);
    if (value >= 1'000) return std::format("{:.1f}k", static_cast<double>(value) / 1e3);
    return std::to_string(value);
}

std::string date_text(const std::string& iso) { return iso.substr(0, std::min<std::size_t>(iso.size(), 10)); }

// Search, category and the sort order, pinned packages first like the site.
// Not safe for work: Thunderstore's own mark on the package, or a category of that name.
bool nsfw(const ts::Package& package) {
    return package.nsfw || std::any_of(package.categories.begin(), package.categories.end(),
                                       [](const std::string& category) { return lower(category) == "nsfw"; });
}

std::vector<const ts::Package*> visible_packages(const Store& store, const ts::Installed& installed, const ts::CategoryFilter& categories) {
    const auto query = lower(store.search.data());
    std::vector<const ts::Package*> result;
    for (const auto& package : store.packages) {
        const bool have = installed.contains(ts::folder_for(package.full_name));
        // NSFW packages are never listed here, installed or not: by Thunderstore's own mark,
        // or a category of that name. A deprecated one only shows once installed.
        if (nsfw(package)) continue;
        if (package.deprecated && !have) continue;
        if (!ts::shown_by(package, categories)) continue;
        if (!query.empty() && lower(package.title()).find(query) == std::string::npos &&
            lower(package.owner).find(query) == std::string::npos &&
            lower(package.latest().description).find(query) == std::string::npos) continue;
        result.push_back(&package);
    }
    const auto order = [&](const ts::Package* a, const ts::Package* b) {
        if (a->pinned != b->pinned) return a->pinned;
        switch (store.sort) {
        case 1: if (a->downloads != b->downloads) return a->downloads > b->downloads; break;
        case 2: if (a->date_created != b->date_created) return a->date_created > b->date_created; break;
        case 3: if (a->rating != b->rating) return a->rating > b->rating; break;
        case 4: break;
        default: if (a->date_updated != b->date_updated) return a->date_updated > b->date_updated; break;
        }
        return lower(a->title()) < lower(b->title());
    };
    std::stable_sort(result.begin(), result.end(), order);
    return result;
}

constexpr std::array<const char*, 5> sort_names{"Last updated", "Most downloaded", "Newest", "Top rated", "Name"};

// GET MODS's categories: Thunderstore's, with how many mods are in each, kept in the launcher's settings.
void category_filter(Launcher& launcher, const Store& store, float width) {
    std::vector<std::pair<std::string, int>> categories;
    for (const auto& package : store.packages) {
        if (nsfw(package)) continue; // (and no category that only they have)
        for (const auto& category : package.categories) ++category_count(categories, category);
    }
    if (category_menu("##category", launcher.settings().mod_categories_only, launcher.settings().mod_categories_hidden, std::move(categories), width))
        launcher.save();
}

// What installing `package` does now: install, update or reinstall.
std::string action_label(const ts::Package& package, const ts::Installed& installed) {
    const auto found = installed.find(ts::folder_for(package.full_name));
    if (found == installed.end()) return "INSTALL";
    if (ts::update_available(package, installed)) return "UPDATE TO v" + package.latest().number;
    return "REINSTALL";
}

} // namespace

int& category_count(std::vector<std::pair<std::string, int>>& categories, const std::string& category) {
    for (auto& entry : categories)
        if (entry.first == category) return entry.second;
    return categories.emplace_back(category, 0).second;
}

bool category_menu(const char* id, std::vector<std::string>& only, std::vector<std::string>& hidden,
                   std::vector<std::pair<std::string, int>> categories, float width) {
    // One chosen before that no mod has now stays in the list, so it can be put back.
    for (const auto* list : {&only, &hidden})
        for (const auto& category : *list) (void)category_count(categories, category);
    // The fullest first: where most of the mods are is what is looked for most.
    std::sort(categories.begin(), categories.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : lower(a.first) < lower(b.first);
    });
    const auto in = [](const std::vector<std::string>& list, const std::string& name) { return std::find(list.begin(), list.end(), name) != list.end(); };
    std::string shown;
    if (only.empty() && hidden.empty()) shown = "All categories";
    else if (hidden.empty()) shown = only.size() == 1 ? only.front() : std::format("{} categories", only.size());
    else if (only.empty()) shown = hidden.size() == 1 ? "Not " + hidden.front() : std::format("{} hidden", hidden.size());
    else shown = std::format("{}, {} hidden", only.size() == 1 ? only.front() : std::format("{} categories", only.size()), hidden.size());
    ImGui::SetNextItemWidth(width);
    // Wider than its button: a row holds a box, the name, how many mods and the eye.
    const float across = std::max(width, S(310));
    ImGui::SetNextWindowSizeConstraints(ImVec2(across, 0), ImVec2(across, S(660)));
    if (!ImGui::BeginCombo(id, shown.c_str(), ImGuiComboFlags_HeightLarge)) return false;
    bool changed = false;
    // What is typed narrows the rows; forgotten each time the menu opens. (One menu is open at a time.)
    static std::array<char, 48> find{};
    if (ImGui::IsWindowAppearing()) {
        find.fill(0);
        if (!ImGui::GetIO().NavVisible) ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##find", "Find a category", find.data(), find.size());
    ImGui::Dummy(ImVec2(0, S(2)));
    const auto query = lower(find.data());
    int most = 1;
    for (const auto& entry : categories) most = std::max(most, entry.second);
    auto* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float tall = S(30), box = S(16), eye = S(26);
    bool any = false;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
    for (const auto& [category, mods] : categories) {
        if (!query.empty() && lower(category).find(query) == std::string::npos) continue;
        any = true;
        const bool is_only = in(only, category), is_hidden = in(hidden, category);
        ImGui::PushID(category.c_str());
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float row = ImGui::GetContentRegionAvail().x;
        // The row ticks its box; the eye at its right, drawn over it, hides the category.
        ImGui::SetNextItemAllowOverlap();
        const bool pressed = ImGui::InvisibleButton("##row", ImVec2(row, tall), ImGuiButtonFlags_EnableNav);
        const bool over = ImGui::IsItemHovered();
        const ImVec2 next = ImGui::GetCursorPos();
        const ImVec2 eye_at(start.x + row - eye - S(2), start.y + (tall - eye) * 0.5f);
        ImGui::SetCursorScreenPos(eye_at);
        const bool hide = ImGui::InvisibleButton("##hide", ImVec2(eye, eye), ImGuiButtonFlags_EnableNav);
        const bool over_eye = ImGui::IsItemHovered();
        ImGui::SetCursorPos(next);
        if (over_eye) ImGui::SetTooltip("%s", is_hidden ? "Hidden. Click to show it again." : "Hide this category");
        else if (over) ImGui::SetTooltip("%s", is_hidden ? "Hidden. Click to show it again."
                                           : is_only ? "Shown only. Click to show every category again."
                                                     : "Show only this category, and any others ticked");
        if (hide) {
            if (is_hidden) std::erase(hidden, category);
            else {
                std::erase(only, category);
                hidden.push_back(category);
            }
            changed = true;
        } else if (pressed) {
            if (is_hidden) std::erase(hidden, category);
            else if (is_only) std::erase(only, category);
            else only.push_back(category);
            changed = true;
        }

        const float middle = start.y + tall * 0.5f;
        if (over || over_eye) draw->AddRectFilled(start, ImVec2(start.x + row, start.y + tall), rgba(255, 255, 255, 0.06f), S(4));
        const ImVec2 at(start.x + S(8), middle - box * 0.5f), corner(at.x + box, at.y + box);
        if (is_only) {
            draw->AddRectFilled(at, corner, color::blue, S(4));
            draw->AddLine(ImVec2(at.x + box * 0.24f, at.y + box * 0.52f), ImVec2(at.x + box * 0.43f, at.y + box * 0.72f), color::ink, S(2));
            draw->AddLine(ImVec2(at.x + box * 0.43f, at.y + box * 0.72f), ImVec2(at.x + box * 0.78f, at.y + box * 0.30f), color::ink, S(2));
        } else {
            draw->AddRectFilled(at, corner, rgba(255, 255, 255, is_hidden ? 0.03f : 0.09f), S(4));
            draw->AddRect(at, corner, rgba(255, 255, 255, is_hidden ? 0.12f : over ? 0.5f : 0.28f), S(4));
        }
        const float name_x = corner.x + S(10), count_end = eye_at.x - S(8);
        const auto count = std::to_string(mods);
        const float count_width = font->CalcTextSizeA(size, FLT_MAX, 0, count.c_str()).x;
        const ImVec4 clip(name_x, start.y, count_end - count_width - S(8), start.y + tall);
        const float text_y = middle - size * 0.5f - S(2);
        draw->AddText(font, size, ImVec2(name_x, text_y), is_hidden ? color::muted : color::text, category.c_str(), nullptr, 0, &clip);
        if (is_hidden) {
            const float name_end = std::min(clip.z, name_x + font->CalcTextSizeA(size, FLT_MAX, 0, category.c_str()).x);
            draw->AddLine(ImVec2(name_x, text_y + size * 0.55f), ImVec2(name_end, text_y + size * 0.55f), color::muted, S(1));
        }
        draw->AddText(font, size, ImVec2(count_end - count_width, text_y), color::muted, count.c_str());
        // How full it is beside the fullest: a line under the name.
        const float full = (count_end - name_x) * static_cast<float>(mods) / static_cast<float>(most);
        draw->AddRectFilled(ImVec2(name_x, start.y + tall - S(5)), ImVec2(name_x + std::max(S(2), full), start.y + tall - S(3)),
                            is_only ? color::blue : rgba(255, 255, 255, is_hidden ? 0.06f : 0.16f), S(1));
        // The eye: open, or struck through in red while the category is hidden.
        const ImVec2 centre(eye_at.x + eye * 0.5f, eye_at.y + eye * 0.5f);
        const ImU32 ink = is_hidden ? color::danger : over_eye ? color::text : rgba(255, 255, 255, 0.3f);
        draw->AddEllipse(centre, ImVec2(S(7.5f), S(4.5f)), ink, 0, 0, S(1.5f));
        draw->AddCircleFilled(centre, S(2), ink);
        if (is_hidden) draw->AddLine(ImVec2(centre.x - S(7), centre.y + S(6)), ImVec2(centre.x + S(7), centre.y - S(6)), ink, S(1.6f));
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (!any) ImGui::TextDisabled("No category by that name.");
    ImGui::Separator();
    ImGui::BeginDisabled(only.empty() && hidden.empty());
    if (ImGui::SmallButton("Clear all")) {
        only.clear();
        hidden.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    const auto standing = only.empty() && hidden.empty() ? std::string("Every category shown")
        : std::format("{}{}{}", only.empty() ? std::string() : std::format("{} shown only", only.size()),
                      only.empty() || hidden.empty() ? "" : ", ", hidden.empty() ? std::string() : std::format("{} hidden", hidden.size()));
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(standing.c_str()).x));
    ImGui::TextDisabled("%s", standing.c_str());
    ImGui::EndCombo();
    return changed;
}

bool category_chips(std::vector<std::string>& only, std::vector<std::string>& hidden) {
    if (only.empty() && hidden.empty()) return false;
    // (Let go of after the loop: the chips are drawn from the lists they change.)
    const std::string* gone = nullptr;
    bool gone_hidden = false;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(13));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(4)));
    auto* draw = ImGui::GetWindowDrawList();
    const float edge = ImGui::GetContentRegionMax().x, cross = S(7);
    bool first = true;
    const auto chip = [&](const std::string& category, bool is_hidden) {
        // Room at its right end for the cross drawn there.
        const auto label = std::format("{}{}      ###category_chip_{}", is_hidden ? "Not " : "", category, category);
        const float chip_width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + S(24);
        if (!first) {
            ImGui::SameLine(0, S(6));
            if (ImGui::GetCursorPosX() + chip_width > edge) ImGui::NewLine();
        }
        first = false;
        if (is_hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
        if (ImGui::Button(label.c_str())) {
            gone = &category;
            gone_hidden = is_hidden;
        }
        if (is_hidden) ImGui::PopStyleColor();
        const bool over = ImGui::IsItemHovered();
        if (over) ImGui::SetTooltip("%s", is_hidden ? "Hidden. Click to show it again." : "Shown only. Click to let it go.");
        const ImVec2 low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        const ImVec2 centre(high.x - S(12) - cross * 0.5f, (low.y + high.y) * 0.5f);
        const ImU32 ink = is_hidden ? color::danger : over ? color::text : color::muted;
        draw->AddLine(ImVec2(centre.x - cross * 0.5f, centre.y - cross * 0.5f), ImVec2(centre.x + cross * 0.5f, centre.y + cross * 0.5f), ink, S(1.5f));
        draw->AddLine(ImVec2(centre.x - cross * 0.5f, centre.y + cross * 0.5f), ImVec2(centre.x + cross * 0.5f, centre.y - cross * 0.5f), ink, S(1.5f));
    };
    for (const auto& category : only) chip(category, false);
    for (const auto& category : hidden) chip(category, true);
    ImGui::PopStyleVar(2);
    if (!gone) return false;
    const auto name = *gone;   // a copy: it is in the list it leaves
    std::erase(gone_hidden ? hidden : only, name);
    return true;
}

// Placed by hand: a row lays its own icon, text and badges out.
void mod_icon(ModsPanel& panel, const thunderstore::Package* package, ImVec2 position, float size,
    const std::filesystem::path& folder) {
    const ImVec2 end(position.x + size, position.y + size);
    auto* draw = ImGui::GetWindowDrawList();
    if (const auto id = package ? package_icon(panel, *package) : !folder.empty() ? folder_icon(panel, folder) : ImTextureID{})
        draw->AddImageRounded(id, position, end, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
    else
        draw->AddRectFilled(position, end, rgba(255, 255, 255, 0.06f), S(4));
}

std::string size_text(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return std::format("{:.2f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    if (bytes >= 1024ull * 1024) return std::format("{:.0f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
    return std::format("{} KB", std::max<std::uint64_t>(1, bytes / 1024));
}

void refresh_listing(ModsPanel& panel, double time, bool force) {
    auto& store = panel.store;
    // Once per launch, unless asked; a failed fetch retries after a minute.
    if (store.loading || (!force && store.fetched > -1e8 && (store.error.empty() || time - store.fetched < 60))) return;
    if (store.worker.joinable()) store.worker.join();
    store.fetched = time;
    store.loading = true;
    store.worker = std::thread([&store] {
        std::vector<ts::Package> packages;
        std::string error;
        const auto community = ts::community();
        try {
            packages = fetch_listing(community, store.stop);
            log(logging::Level::info, std::format("Thunderstore: {} package(s) in {}", packages.size(), community));
        } catch (const std::exception& failure) {
            error = failure.what();
            if (!store.stop) log(logging::Level::warning, "Thunderstore listing could not be read: " + error);
        }
        std::lock_guard lock(store.mutex);
        store.incoming = std::move(packages);
        store.incoming_error = std::move(error);
        store.arrived = true;
        store.loading = false;
    });
}

void collect_listing(ModsPanel& panel) {
    auto& store = panel.store;
    std::lock_guard lock(store.mutex);
    if (!store.arrived) return;
    store.arrived = false;
    if (store.incoming_error.empty()) {
        store.packages = std::move(store.incoming);
        store.loaded = true;
        panel.categories_by_folder.clear();
        for (const auto& package : store.packages) panel.categories_by_folder[ts::folder_for(package.full_name)] = package.categories;
        store.error.clear();
    } else {
        store.error = std::move(store.incoming_error);
    }
    store.incoming.clear();
}

thunderstore::Installed installed_versions(const mods::ModList& list, bool enabled_only) {
    ts::Installed installed;
    for (const auto& entry : list.entries)
        if (entry.enabled || !enabled_only) installed[entry.mod.name] = entry.mod.version;
    return installed;
}

const thunderstore::Package* package_for(const Store& store, std::string_view folder) {
    for (const auto& package : store.packages)
        if (ts::folder_for(package.full_name) == folder) return &package;
    return nullptr;
}

std::vector<const thunderstore::Package*> updates(const Store& store, const thunderstore::Installed& installed) {
    std::vector<const ts::Package*> result;
    for (const auto& package : store.packages)
        if (ts::update_available(package, installed))
            result.push_back(&package);
    return result;
}

namespace {
// The texture for an icon URL (or local_icon_prefix and a path), or empty while the worker loads it.
ImTextureID icon_texture(Icons& icons, const std::string& url) {
    if (url.empty() || !g_renderer) return {};
    auto& entry = icons.entries[url];
    entry.used = ImGui::GetFrameCount();
    if (entry.id || entry.failed || entry.queued) return entry.id;
    entry.queued = true;
    {
        std::lock_guard lock(icons.mutex);
        icons.queue.push_back(url);
    }
    if (!icons.worker.joinable()) icons.worker = std::thread([&icons] { icon_worker(icons); });
    icons.wake.notify_one();
    return {};
}
} // namespace

ImTextureID package_icon(ModsPanel& panel, const thunderstore::Package& package) {
    return icon_texture(panel.icons, package.latest().icon);
}

ImTextureID folder_icon(ModsPanel& panel, const std::filesystem::path& folder) {
    // A Thunderstore package ships its icon.png beside manifest.json, so a mod installed by hand
    // or not (yet) on Thunderstore usually has one too.
    const auto path = utf8((folder / L"icon.png").wstring());
    return icon_texture(panel.icons, path.empty() ? path : std::string(local_icon_prefix) + path);
}

void pump_icons(ModsPanel& panel) {
    auto& icons = panel.icons;
    std::vector<Icons::Decoded> done;
    {
        std::lock_guard lock(icons.mutex);
        done.swap(icons.done);
    }
    if (!g_renderer) return;
    for (auto& decoded : done) {
        auto& entry = icons.entries[decoded.url];
        entry.queued = false;
        if (decoded.pixels.empty()) { entry.failed = true; continue; }
        // Make room by dropping the icons drawn longest ago.
        std::size_t loaded = 0;
        for (const auto& [url, other] : icons.entries) loaded += other.id ? 1 : 0;
        while (loaded >= max_icons) {
            auto oldest = icons.entries.end();
            for (auto it = icons.entries.begin(); it != icons.entries.end(); ++it)
                if (it->second.id && (oldest == icons.entries.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == icons.entries.end() || oldest->second.used >= ImGui::GetFrameCount() - 1) break;
            g_renderer->release_texture(oldest->second.id);
            oldest->second.id = {};
            --loaded;
        }
        entry.id = g_renderer->upload_texture(decoded.pixels, decoded.width, decoded.height);
        if (!entry.id) entry.failed = true;
    }
}

bool picked(const Store& store, const std::string& full_name) {
    return std::find(store.picked.begin(), store.picked.end(), full_name) != store.picked.end();
}

void pick(Store& store, const std::string& full_name, bool on) {
    const auto found = std::find(store.picked.begin(), store.picked.end(), full_name);
    if (on && found == store.picked.end()) store.picked.push_back(full_name);
    else if (!on && found != store.picked.end()) store.picked.erase(found);
}

void browse_page(Launcher& launcher, const Fonts& fonts, ModsPanel& panel, float height, bool installing) {
    auto& store = panel.store;
    const auto installed = installed_versions(panel.list);

    const float top = ImGui::GetCursorPosY();
    // ------------------------------------------------ search, category, sort, refresh
    const float combo = S(190), refresh = S(110);
    ImGui::SetNextItemWidth(std::max(S(140),
        ImGui::GetContentRegionAvail().x - (combo + S(10)) * 2 - refresh - S(10)));
    // Ctrl+F jumps to the search, as it does in a browser.
    if (!installing && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Search mods  (Ctrl+F)", store.search.data(), store.search.size());
    ImGui::SameLine();
    category_filter(launcher, store, combo);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo);
    if (ImGui::BeginCombo("##sort", sort_names[static_cast<std::size_t>(std::clamp(store.sort, 0, 4))])) {
        for (int i = 0; i < static_cast<int>(sort_names.size()); ++i)
            if (ImGui::Selectable(sort_names[static_cast<std::size_t>(i)], store.sort == i)) store.sort = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(store.loading);
    if (ImGui::Button("Refresh", ImVec2(refresh, 0))) refresh_mods(launcher, panel);
    ImGui::EndDisabled();
    // What the category menu is set to, where it can be seen with the menu shut.
    if (category_chips(launcher.settings().mod_categories_only, launcher.settings().mod_categories_hidden)) launcher.save();

    // ------------------------------------------------ what is ticked
    if (!store.picked.empty()) {
        ImGui::Spacing();
        const ImVec2 bar = ImGui::GetCursorScreenPos();
        const float pad = S(10), bar_tall = ImGui::GetFrameHeight() + pad * 2;
        card(ImGui::GetWindowDrawList(), bar, ImVec2(ImGui::GetContentRegionAvail().x, bar_tall), true);
        ImGui::SetCursorScreenPos(ImVec2(bar.x + S(20), bar.y + pad));
        ImGui::AlignTextToFramePadding();
        // What installing them all downloads: each one's newest version.
        std::uint64_t bytes{};
        for (const auto& name : store.picked)
            for (const auto& package : store.packages)
                if (package.full_name == name) bytes += package.latest().file_size;
        auto chosen_text = store.picked.size() == 1 ? std::string("1 mod selected")
                                                    : std::format("{} mods selected", store.picked.size());
        if (bytes) chosen_text += "  /  " + size_text(bytes) + " to download";
        ImGui::TextUnformatted(chosen_text.c_str());
        ImGui::SameLine();
        const float action = S(190);
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - action - S(90) - S(8) - pad);
        if (ImGui::Button("Clear", ImVec2(S(90), 0))) store.picked.clear();
        ImGui::SameLine();
        ImGui::BeginDisabled(installing);
        push_primary_button();
        if (ImGui::Button(store.picked.size() == 1 ? "INSTALL 1 MOD"
                : std::format("INSTALL {} MODS", store.picked.size()).c_str(), ImVec2(action, 0))) {
            std::vector<ts::Package> chosen;
            for (const auto& name : store.picked)
                for (const auto& package : store.packages)
                    if (package.full_name == name) chosen.push_back(package);
            store.picked.clear();
            start_store_install(panel, std::move(chosen));
        }
        pop_primary_button();
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(bar.x, bar.y + bar_tall));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Spacing();
    const float body = std::max(S(120), height - (ImGui::GetCursorPosY() - top));
    const auto packages = visible_packages(store, installed, {launcher.settings().mod_categories_only, launcher.settings().mod_categories_hidden});

    // ------------------------------------------------ the list
    ImGui::BeginChild("##store_list", ImVec2(0, body), ImGuiChildFlags_NavFlattened);
    const auto note = [](const char* text) {
        ImGui::Spacing();
        ImGui::Indent(S(14));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(S(14));
    };
    if (!store.loaded) {
        if (store.loading || store.error.empty()) note("Loading mods from Thunderstore...");
        else {
            ImGui::Spacing();
            ImGui::Indent(S(14));
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger), "Thunderstore could not be reached: %s",
                store.error.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Try again")) refresh_listing(panel, ImGui::GetTime(), true);
            ImGui::Unindent(S(14));
        }
    } else if (store.packages.empty()) {
        note("No mods on Thunderstore yet. Made one? Package it with a manifest.json, icon.png and README.md "
             "and upload it to thunderstore.io/c/reskate.");
    } else if (packages.empty()) {
        note("No mods match your search.");
    }

    // A row: the box that ticks it and its icon at the left, its name over two lines of what it
    // says it is, and at the right one column, the same width in every row, for what is done to it.
    const float row = S(98) + row_gap();
    ImGui::BeginDisabled(installing);
    virtual_rows(static_cast<int>(packages.size()), [&](int) { return row; }, [&](int index, float slot) {
        const float tall = slot - row_gap();
        const auto& package = *packages[static_cast<std::size_t>(index)];
        const auto& version = package.latest();
        const auto found = installed.find(ts::folder_for(package.full_name));
        const bool update = ts::update_available(package, installed);
        // Tools (Blender add-ons, utilities) are not game mods: their page has the download.
        const bool tool = package.in_category("Tools") && !package.in_category("Mods");
        const bool ticked = picked(store, package.full_name);

        ImGui::PushID(package.full_name.c_str());
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        // The row itself opens the overview; the widgets on it keep their clicks.
        begin_row();
        if (list_row("##row", width, tall, ticked)) {
            store.selected = package.full_name;
            store.overview = false;
        }
        row_buttons();
        const float right = start.x + width;
        const float icon = S(64), icon_x = start.x + S(46), text_x = icon_x + icon + S(16);
        // Ticked, to install with the others ticked (a tool has no box: its page has the download).
        if (!tool) {
            if (tick_box("##pick", ImVec2(start.x + S(14), start.y + (tall - tick_box_size()) * 0.5f), ticked))
                pick(store, package.full_name, !ticked);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tick to install this with the others you tick");
        }
        mod_icon(panel, &package, ImVec2(icon_x, start.y + (tall - icon) * 0.5f), icon);

        // ------------------------------------------- what is done to it, in a column at the right
        const float button = S(132), button_tall = S(32);
        const float button_x = right - S(16) - button;
        ImGui::SetCursorScreenPos(ImVec2(button_x, start.y + (tall - button_tall) * 0.5f));
        if (tool) {
            if (ImGui::Button("STORE PAGE", ImVec2(button, button_tall))) open_url(package.package_url);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("A tool, not a game mod: get it from its Thunderstore page.");
        } else if (const auto coming = download_label(panel, package.full_name); !coming.empty()) {
            // In the queue already: the button says where it is, and is not to be pressed again.
            ImGui::BeginDisabled();
            ImGui::Button(coming.c_str(), ImVec2(button, button_tall));
            ImGui::EndDisabled();
        } else if (update) {
            push_primary_button();
            if (ImGui::Button("UPDATE", ImVec2(button, button_tall))) start_store_install(panel, {package});
            pop_primary_button();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thunderstore has v%s", version.number.c_str());
        } else if (found == installed.end()) {
            push_primary_button();
            if (ImGui::Button("INSTALL", ImVec2(button, button_tall))) start_store_install(panel, {package});
            pop_primary_button();
        } else {
            // Installed and up to date: said in the middle of the column, where the button would be.
            const auto mark = "INSTALLED";
            badge(draw, fonts, ImVec2(button_x + (button - badge_width(fonts, mark)) * 0.5f,
                start.y + (tall - fonts.caption->FontSize - S(8)) * 0.5f), mark, color::good, color::ink);
        }

        const float limit = button_x - S(18);
        const ImVec4 title_clip(text_x, start.y, limit, start.y + tall);
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(text_x, start.y + S(10)), color::text,
            package.title().c_str(), nullptr, 0, &title_clip);
        const float title_width =
            fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, package.title().c_str()).x;
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x + title_width + S(8), start.y + S(12)),
            color::muted, ("by " + package.owner).c_str(), nullptr, 0, &title_clip);

        auto line = version.description;
        std::replace_if(line.begin(), line.end(),
            [](char ch) { return ch == 0x0a || ch == 0x0d || ch == 0x09; }, ' ');
        // Two lines, wrapped, and cut off after them, so every row is the same height.
        const float described = start.y + S(33);
        const ImVec4 clip(text_x, described, limit, described + fonts.body->FontSize * 2 + S(1));
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, described), color::text, line.c_str(),
            nullptr, limit - text_x, &clip);
        draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(text_x, start.y + tall - fonts.caption->FontSize - S(10)), color::muted,
            std::format("v{}{}  /  {} downloads  /  {}", version.number,
                version.file_size ? "  /  " + size_text(version.file_size) : std::string(),
                count_text(package.downloads), date_text(package.date_updated)).c_str());
        end_row();
        ImGui::PopID();
    });
    ImGui::EndDisabled();
    keep_focus_in_list();
    ImGui::EndChild();
}

namespace {

// ---------------------------------------------------------------- READMEs

constexpr std::uint64_t max_readme_bytes = 512 * 1024;

void readme_worker(Readmes& readmes) {
    std::atomic<bool> never{};
    for (;;) {
        Readmes::Job job;
        {
            std::unique_lock lock(readmes.mutex);
            readmes.wake.wait(lock, [&] { return readmes.stop || !readmes.queue.empty(); });
            if (readmes.stop) break;
            job = std::move(readmes.queue.front());
            readmes.queue.pop_front();
        }
        Readmes::Done done{job.key, {}, false};
        try {
            done.markdown = ts::parse_readme(fetch(job.url, max_readme_bytes, never));
        } catch (const std::exception& failure) {
            done.failed = true;
            logging::write(logging::Level::warning, logging::Channel::launcher,
                std::string("Could not get the README of ") + job.key + ": " + failure.what());
        }
        std::lock_guard lock(readmes.mutex);
        readmes.done.push_back(std::move(done));
    }
}

// The entry to draw, asked for or read the first time. Null: nothing to show.
const Readmes::Entry* readme_for(ModsPanel& panel, const ts::Package* package, const mods::Mod* mod) {
    auto& readmes = panel.readmes;
    {
        std::lock_guard lock(readmes.mutex);
        for (auto& done : readmes.done) {
            auto& entry = readmes.entries[done.key];
            entry.loading = false;
            entry.failed = done.failed;
            entry.readme = ts::readme_lines(done.markdown);
        }
        readmes.done.clear();
    }
    // An installed mod's own README.md is what is on this PC, so it comes first.
    if (mod) {
        const auto key = "folder:" + mod->name + ":" + mod->version;
        auto found = readmes.entries.find(key);
        if (found == readmes.entries.end()) {
            Readmes::Entry entry;
            std::error_code error;
            const auto file = mod->directory / "README.md";
            if (const auto size = fs::file_size(file, error); !error && size > 0 && size <= max_readme_bytes) {
                const auto bytes = read_bytes(file);
                std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
                entry.readme = ts::readme_lines(text);
            }
            found = readmes.entries.emplace(key, std::move(entry)).first;
        }
        if (!found->second.readme.lines.empty()) return &found->second;
    }
    if (!package) return nullptr;
    const auto key = package->latest().full_name;
    auto found = readmes.entries.find(key);
    if (found == readmes.entries.end()) {
        found = readmes.entries.emplace(key, Readmes::Entry{true, false, {}}).first;
        {
            std::lock_guard lock(readmes.mutex);
            readmes.queue.push_back({key, ts::readme_url(*package)});
        }
        if (!readmes.worker.joinable()) readmes.worker = std::thread([&readmes] { readme_worker(readmes); });
        readmes.wake.notify_one();
    }
    return &found->second;
}

} // namespace

// The two columns of an overview, under its header: the README on the left, the facts in a panel
// of their own on the right. Both are as tall as the room above the overview's last row.
OverviewColumns overview_columns(float popup_height) {
    OverviewColumns columns;
    columns.gap = S(18);
    columns.height = std::max(S(140), popup_height - ImGui::GetCursorPosY() - S(24) - ImGui::GetFrameHeight() - S(12));
    const float room = ImGui::GetContentRegionAvail().x;
    columns.details = std::clamp(room * 0.32f, S(220), S(320));
    columns.readme = room - columns.details - columns.gap;
    return columns;
}

void begin_overview_details(const char* id, const OverviewColumns& columns) {
    ImGui::SameLine(0, columns.gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(rgba(31, 31, 34)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild(id, ImVec2(columns.details, columns.height),
        ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
}

void end_overview_details() {
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void overview_fact(const Fonts& fonts, const char* name, const std::string& value) {
    if (value.empty()) return;
    field(fonts, name, value);
    ImGui::Dummy(ImVec2(0, S(6)));
}

void draw_readme(const Fonts& fonts, const thunderstore::Readme& readme) {
    using Kind = ts::ReadmeLine::Kind;
    ImGui::PushTextWrapPos(0);
    for (const auto& line : readme.lines) {
        switch (line.kind) {
        case Kind::heading:
            if (&line != &readme.lines.front()) ImGui::Dummy(ImVec2(0, S(8)));
            ImGui::PushFont(fonts.bold);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopFont();
            break;
        case Kind::bullet:
            ImGui::Bullet();
            ImGui::TextUnformatted(line.text.c_str());
            break;
        case Kind::code:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            break;
        case Kind::rule: ImGui::Separator(); break;
        case Kind::gap: ImGui::Spacing(); break;
        case Kind::text: ImGui::TextUnformatted(line.text.c_str()); break;
        }
    }
    ImGui::PopTextWrapPos();
}

bool readme_field(const Fonts& fonts, ModsPanel& panel, const thunderstore::Package* package, const mods::Mod* mod) {
    const auto* entry = readme_for(panel, package, mod);
    if (!entry || (!entry->loading && !entry->failed && entry->readme.lines.empty())) return false;
    if (entry->loading) {
        ImGui::TextDisabled("Loading the README...");
        return true;
    }
    if (entry->failed) {
        ImGui::TextDisabled("Could not load the README. It is on the mod's Thunderstore page.");
        return true;
    }
    draw_readme(fonts, entry->readme);
    if (entry->readme.cut) ImGui::TextDisabled("The rest is on the mod's Thunderstore page.");
    return true;
}

void package_overview(const Fonts& fonts, ModsPanel& panel, ImVec2 size, bool installing) {
    auto& store = panel.store;
    if (store.selected.empty()) return;
    const ts::Package* selected = nullptr;
    for (const auto& package : store.packages)
        if (package.full_name == store.selected) selected = &package;
    if (!selected) {
        store.selected.clear();
        return;
    }
    const auto& package = *selected;
    const auto& version = package.latest();
    const auto installed = installed_versions(panel.list);
    const auto found = installed.find(ts::folder_for(package.full_name));

    if (!store.overview) {
        ImGui::OpenPopup("##package_overview");
        store.overview = true;
    }
    const ImVec2 extent(std::min(S(1080), size.x - S(80)), std::min(S(740), size.y - S(60)));
    ImGui::SetNextWindowPos(ImVec2((size.x - extent.x) * 0.5f, (size.y - extent.y) * 0.5f));
    ImGui::SetNextWindowSize(extent);
    if (!ImGui::BeginPopupModal("##package_overview", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        store.overview = false;        // dismissed with Escape
        store.selected.clear();
        return;
    }
    const auto close = [&] {
        store.overview = false;
        store.selected.clear();
        ImGui::CloseCurrentPopup();
    };
    // The icon, with the name, who made it and its one-line description beside it.
    const float icon = S(104), header_top = ImGui::GetCursorPosY();
    mod_icon(panel, &package, ImGui::GetCursorScreenPos(), icon);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + icon + S(20));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(package.title().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("by %s", package.owner.c_str());
    if (!version.description.empty()) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextUnformatted(version.description.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), header_top + icon) + S(14));

    const bool tool = package.in_category("Tools") && !package.in_category("Mods");
    if (tool) {
        push_primary_button();
        if (ImGui::Button("GET IT ON THUNDERSTORE", ImVec2(0, S(34)))) open_url(package.package_url);
        pop_primary_button();
    } else {
        const auto coming = download_label(panel, package.full_name);
        ImGui::BeginDisabled(installing || !coming.empty());
        push_primary_button();
        if (ImGui::Button(coming.empty() ? action_label(package, installed).c_str() : coming.c_str(), ImVec2(0, S(34)))) {
            start_store_install(panel, {package});
            close();
        }
        pop_primary_button();
        ImGui::EndDisabled();
        if (!package.package_url.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Thunderstore page", ImVec2(0, S(34)))) open_url(package.package_url);
        }
    }
    if (version.website_url.starts_with("https://")) {
        ImGui::SameLine();
        if (ImGui::Button("Website", ImVec2(0, S(34)))) open_url(version.website_url);
    }
    ImGui::Spacing();

    ImGui::Dummy(ImVec2(0, S(6)));
    const auto columns = overview_columns(extent.y);
    ImGui::BeginChild("##overview_body", ImVec2(columns.readme, columns.height), ImGuiChildFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
    if (package.deprecated)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning),
            "Deprecated: its author no longer supports it.");
    if (package.nsfw)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning), "Marked as not safe for work.");
    if (!readme_field(fonts, panel, &package, nullptr)) ImGui::TextDisabled("This mod has no README.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    begin_overview_details("##overview_details", columns);
    overview_fact(fonts, "VERSION", "v" + version.number);
    if (found != installed.end())
        overview_fact(fonts, "INSTALLED", found->second.empty() ? std::string("yes") : "v" + found->second);
    overview_fact(fonts, "DOWNLOADS", count_text(package.downloads));
    overview_fact(fonts, "UPDATED", date_text(package.date_updated));
    if (version.file_size) overview_fact(fonts, "SIZE", size_text(version.file_size));
    std::string names;
    for (const auto& category : package.categories) names += (names.empty() ? "" : ", ") + category;
    overview_fact(fonts, "CATEGORIES", names);
    overview_fact(fonts, "FOLDER", "Mods\\" + ts::folder_for(package.full_name));
    end_overview_details();

    ImGui::SetCursorPosY(extent.y - S(24) - ImGui::GetFrameHeight());
    bool ticked = picked(store, package.full_name);
    ImGui::BeginDisabled(tool);
    if (ImGui::Checkbox("Install with the others I tick", &ticked)) pick(store, package.full_name, ticked);
    ImGui::EndDisabled();
    ImGui::SameLine(extent.x - S(28) - S(110));
    // Escape too, and so a controller's B: the modal has no other way out but CLOSE.
    if (ImGui::Button("CLOSE", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close();
    ImGui::EndPopup();
}

} // namespace dingosdk::launcher_gui::detail
