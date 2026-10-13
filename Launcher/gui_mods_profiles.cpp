#include "gui_internal.h"

#include "mod_profiles.h"
#include "updater.h"

#include "Engine/Core/Log/logging.h"

#include <format>
#include <fstream>

// The Mods panel's PROFILES page: named sets of mods to switch between, and
// sharing them as r2modman does, by code or by file (mod_profiles.h). Every mod
// stays installed once; loading a profile rewrites mods.json and downloads what
// it names that is missing.
namespace dingosdk::launcher_gui::detail {
namespace {

namespace profiles = launcher_profiles;
namespace ts = thunderstore;

constexpr int code_timeout_ms = 20000;
constexpr std::uint64_t max_code_answer = 8 * 1024 * 1024, max_profile_file = 64ull * 1024 * 1024;

void log(logging::Level level, const std::string& message) {
    logging::write(level, logging::Channel::launcher, message);
}

void say(ModsPanel& panel, std::string message, bool error = false) {
    panel.message = std::move(message);
    panel.message_error = error;
}

bool store(ModsPanel& panel) {
    try {
        profiles::save(panel.root, panel.profiles);
        return true;
    } catch (const std::exception& failure) {
        say(panel, failure.what(), true);
        return false;
    }
}

std::string trimmed(const char* text) {
    std::string value(text);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

// "a, b, c and 4 more".
std::string named(const std::vector<std::string>& names) {
    constexpr std::size_t shown = 3;
    std::string text;
    for (std::size_t i = 0; i < names.size() && i < shown; ++i) text += (i ? ", " : "") + names[i];
    if (names.size() > shown) text += std::format(" and {} more", names.size() - shown);
    return text;
}

// The profile's mods that are on, missing, and not in the Thunderstore listing either.
std::vector<std::string> unobtainable(const ModsPanel& panel, const std::vector<profiles::ProfileMod>& missing) {
    std::vector<std::string> names;
    for (const auto& mod : missing)
        if (!package_for(panel.store, mod.name)) names.push_back(mod.name);
    return names;
}

// Writes mods.json as the profile has it. False, with the reason shown, when that failed.
bool write_order(ModsPanel& panel, const profiles::Profile& profile, std::vector<profiles::ProfileMod>& missing) {
    auto applied = profiles::apply(profile, panel.list);
    try {
        mods::save_mod_order(panel.root, applied.entries);
    } catch (const std::exception& failure) {
        say(panel, failure.what(), true);
        return false;
    }
    panel.list.entries = std::move(applied.entries);
    panel.list.issue.clear();
    panel.marked.clear();
    panel.selected = -1;
    missing = std::move(applied.missing);
    return true;
}

void load_profile(ModsPanel& panel, const profiles::Profile& profile) {
    std::vector<profiles::ProfileMod> missing;
    if (!write_order(panel, profile, missing)) return;
    panel.profiles.active = profile.name;
    if (!store(panel)) return;
    log(logging::Level::info, std::format("Mod profile loaded: {} ({} mods, {} to download)", profile.name,
        profile.mods.size(), missing.size()));
    if (missing.empty()) {
        say(panel, "Loaded \"" + profile.name + "\". Changes apply the next time Skate starts.");
        return;
    }
    if (!panel.store.loaded) {
        say(panel, std::format("Loaded \"{}\", but {} of its mods are not installed and the Thunderstore listing has not "
            "arrived yet. Load it again in a moment to download them.", profile.name, missing.size()), true);
        return;
    }
    std::vector<ts::Package> packages;
    for (const auto& mod : missing)
        if (const auto* package = package_for(panel.store, mod.name))
            packages.push_back(package->at_version(mod.version));
    if (packages.empty()) {
        say(panel, std::format("Loaded \"{}\". {} of its mods are not installed and are not on Thunderstore: {}.",
            profile.name, missing.size(), named(unobtainable(panel, missing))), true);
        return;
    }
    // The downloaded mods arrive switched on at the end of the list; settle_profile puts
    // them where the profile has them once the install is done.
    panel.profile_settle = profile.name;
    start_store_install(panel, std::move(packages));
}

void add_profile(ModsPanel& panel, profiles::Profile profile, std::string_view verb) {
    if (panel.profiles.list.size() >= profiles::max_profiles) {
        say(panel, std::format("There is room for {} profiles. Delete one first.", profiles::max_profiles), true);
        return;
    }
    profile.name = panel.profiles.free_name(profiles::clean_profile_name(profile.name));
    const auto name = profile.name;
    const auto count = profile.mods.size();
    const auto missing = profiles::apply(profile, panel.list).missing.size();
    panel.profiles.list.push_back(std::move(profile));
    if (!store(panel)) {
        panel.profiles.list.pop_back();
        return;
    }
    log(logging::Level::info, std::format("Mod profile {}: {} ({} mods)", verb, name, count));
    say(panel, missing ? std::format("{} \"{}\" ({} mods). LOAD it to switch to it; {} of its mods will be downloaded.",
                                     verb, name, count, missing)
                       : std::format("{} \"{}\" ({} mods). LOAD it to switch to it.", verb, name, count));
}

std::string read_file(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > max_profile_file) throw std::runtime_error("That file is too large to be a mod profile.");
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read " + utf8(path.wstring()));
    return bytes;
}

// Runs `task` off the UI thread; what it returns is shown by collect_profile.
template<class Task> void in_background(ModsPanel& panel, Task task) {
    if (panel.profile_busy) return;
    if (panel.profile_worker.joinable()) panel.profile_worker.join();
    panel.profile_busy = true;
    panel.message.clear();
    panel.profile_worker = std::thread([&panel, task = std::move(task)] {
        ModsPanel::ProfileResult result;
        try {
            result = task();
        } catch (const std::exception& failure) {
            result.message = failure.what();
            result.error = true;
        }
        result.ready = true;
        std::lock_guard lock(panel.mutex);
        panel.profile_result = std::move(result);
        panel.profile_busy = false;
    });
}

void export_code(ModsPanel& panel, const profiles::Profile& profile) {
    const auto missing_from_store = [&] {
        std::size_t count{};
        for (const auto& mod : profile.mods)
            if (mod.enabled && !package_for(panel.store, mod.name)) ++count;
        return panel.store.loaded ? count : 0;
    }();
    in_background(panel, [profile, missing_from_store] {
        const auto answer = launcher_update::http_post(profiles::code_create_url(), L"application/octet-stream",
            profiles::code_payload(profile), 64 * 1024, code_timeout_ms);
        ModsPanel::ProfileResult result;
        result.clipboard = profiles::parse_created_code(answer);
        result.message = "The code for \"" + profile.name + "\" is on the clipboard: " + result.clipboard +
            ". Thunderstore keeps codes for a limited time.";
        if (missing_from_store)
            result.message += std::format(" {} of its mods are not on Thunderstore, so whoever imports it will not get those.",
                missing_from_store);
        log(logging::Level::info, "Mod profile exported as a code: " + profile.name);
        return result;
    });
}

void import_code(ModsPanel& panel, std::string code) {
    in_background(panel, [code = std::move(code)] {
        std::string payload;
        launcher_update::http_stream(profiles::code_get_url(code), max_code_answer, code_timeout_ms,
            [&](const char* data, DWORD size) { payload.append(data, size); });
        ModsPanel::ProfileResult result;
        result.imported = profiles::parse_code_payload(payload);
        return result;
    });
}

void export_file(ModsPanel& panel, const profiles::Profile& profile, HWND window) {
    const auto folder = pick_path(window, true, L"Choose where to save the profile file", nullptr, nullptr);
    if (folder.empty()) return;
    try {
        auto target = folder / (wide(profile.name) + L".r2z");
        for (int number = 2; fs::exists(target); ++number)
            target = folder / (wide(profile.name) + std::format(L" ({}).r2z", number));
        const auto bytes = profiles::export_archive(profile);
        std::ofstream output(target, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output.flush()) throw std::runtime_error("Could not write " + utf8(target.wstring()));
        log(logging::Level::info, "Mod profile exported as a file: " + profile.name);
        say(panel, "Saved " + utf8(target.wstring()) + ". Anyone can import it here or in r2modman.");
    } catch (const std::exception& failure) {
        say(panel, failure.what(), true);
    }
}

void import_file(ModsPanel& panel, HWND window) {
    const auto path = pick_path(window, false, L"Choose a profile file to import", L"Mod profile (*.r2z; *.r2x)", L"*.r2z;*.r2x");
    if (path.empty()) return;
    try {
        add_profile(panel, profiles::parse_archive(read_file(path)), "Imported");
    } catch (const std::exception& failure) {
        say(panel, failure.what(), true);
    }
}

// Takes over what the background task finished.
void collect_profile(ModsPanel& panel) {
    ModsPanel::ProfileResult result;
    {
        std::lock_guard lock(panel.mutex);
        if (!panel.profile_result.ready) return;
        result = std::exchange(panel.profile_result, {});
    }
    if (!result.clipboard.empty()) ImGui::SetClipboardText(result.clipboard.c_str());
    if (result.imported) add_profile(panel, std::move(*result.imported), "Imported");
    else say(panel, std::move(result.message), result.error);
}

} // namespace

void settle_profile(ModsPanel& panel) {
    // Read with the page, so the rail can count them before PROFILES is opened.
    if (!panel.profiles_loaded && !panel.root.empty()) {
        panel.profiles = profiles::load(panel.root);
        panel.profiles_loaded = true;
    }
    if (panel.profile_settle.empty() || panel.installing) return;
    {
        std::lock_guard lock(panel.mutex);
        if (panel.finished) return;   // collect_install has not rescanned yet
    }
    const auto name = std::exchange(panel.profile_settle, {});
    const auto* profile = panel.profiles.find(name);
    if (!profile) return;
    // A failed download has already said why; keep that on screen.
    const auto failure = panel.message_error ? panel.message : std::string();
    std::vector<profiles::ProfileMod> missing;
    if (!write_order(panel, *profile, missing)) return;
    if (!failure.empty()) say(panel, "Loaded \"" + name + "\", but not all of it: " + failure, true);
    else if (!missing.empty())
        say(panel, std::format("Loaded \"{}\". {} of its mods are not installed and are not on Thunderstore: {}.", name,
            missing.size(), named(unobtainable(panel, missing))), true);
    else say(panel, "Loaded \"" + name + "\" and downloaded what it needed. Changes apply the next time Skate starts.");
}

void profiles_page(const Fonts& fonts, ModsPanel& panel, HWND window, float height, bool installing) {
    collect_profile(panel);
    const bool busy = installing || panel.profile_busy;
    const float top = ImGui::GetCursorPosY();

    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("A profile remembers which of your mods are on and in what order. Every mod stays installed "
                        "once; loading a profile switches the rest off. Share one as a code or a file, with ReSkate "
                        "or r2modman.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    ImGui::BeginDisabled(busy);
    // ------------------------------------------------ new, and the two ways in
    const float button = S(280), gap = S(10);
    ImGui::SetNextItemWidth(std::max(S(160), ImGui::GetContentRegionAvail().x - button - gap));
    const bool entered_name = ImGui::InputTextWithHint("##profile_name", "Name for a new profile", panel.profile_name.data(),
        panel.profile_name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, gap);
    push_primary_button();
    const bool create = ImGui::Button("SAVE CURRENT MODS", ImVec2(button, 0)) || entered_name;
    pop_primary_button();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save what MY MODS shows now as a new profile");
    if (create) {
        const auto name = trimmed(panel.profile_name.data());
        if (!profiles::valid_profile_name(name))
            say(panel, "Give the profile a name first (up to 48 characters, none of < > : \" / \\ | ? *).", true);
        else if (panel.profiles.find(name)) say(panel, "A profile named \"" + name + "\" already exists.", true);
        else {
            const auto before = panel.profiles.list.size();
            add_profile(panel, profiles::capture(name, panel.list), "Saved");
            if (panel.profiles.list.size() > before) {
                panel.profiles.active = panel.profiles.list.back().name;
                store(panel);
                panel.profile_name.fill(0);
            }
        }
    }
    const float half = (button - gap) * 0.5f;
    ImGui::SetNextItemWidth(std::max(S(160), ImGui::GetContentRegionAvail().x - button - gap));
    const bool entered_code = ImGui::InputTextWithHint("##profile_code", "Paste a profile code", panel.profile_code.data(),
        panel.profile_code.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, gap);
    if (ImGui::Button("IMPORT CODE", ImVec2(half, 0)) || entered_code) {
        const auto code = trimmed(panel.profile_code.data());
        if (!profiles::valid_code(code)) say(panel, "That is not a profile code. Paste the whole code you were given.", true);
        else {
            import_code(panel, code);
            panel.profile_code.fill(0);
        }
    }
    ImGui::SameLine(0, gap);
    if (ImGui::Button("IMPORT FILE", ImVec2(half, 0))) import_file(panel, window);
    ImGui::EndDisabled();
    if (panel.profile_busy) {
        ImGui::SameLine(0, gap);
        ImGui::TextDisabled("Talking to Thunderstore...");
    }

    // ------------------------------------------------ the profiles
    ImGui::Spacing();
    ImGui::BeginChild("##profiles", ImVec2(0, height - (ImGui::GetCursorPosY() - top)), ImGuiChildFlags_NavFlattened);
    if (panel.profiles.list.empty()) {
        ImGui::Dummy(ImVec2(0, S(24)));
        ImGui::TextDisabled("No profiles yet. Set up MY MODS the way you like, name it above and save it.");
    }
    // What a row asked for, carried out after the list is drawn: most of it changes the list.
    enum class Ask { none, load, update, code, file, rename, remove } ask{Ask::none};
    std::size_t asked{};
    for (std::size_t index = 0; index < panel.profiles.list.size(); ++index) {
        const auto& profile = panel.profiles.list[index];
        ImGui::PushID(static_cast<int>(index));
        const auto on = std::count_if(profile.mods.begin(), profile.mods.end(), [](const auto& mod) { return mod.enabled; });
        const auto missing = profiles::apply(profile, panel.list).missing.size();
        const bool current = profiles::in_use(profile, panel.list);
        const bool last = !current && profile.name == panel.profiles.active;

        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted(profile.name.c_str());
        ImGui::PopFont();
        if (current || last) {
            ImGui::SameLine(0, S(12));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(current ? color::good : color::warning), "%s",
                current ? "IN USE" : "CHANGED SINCE LOADED");
        }
        ImGui::TextDisabled("%s", (std::format("{} mods, {} on", profile.mods.size(), on) +
            (missing ? std::format("  |  {} to download", missing) : std::string())).c_str());
        ImGui::BeginDisabled(busy);
        const auto act = [&](const char* label, Ask what, const char* tip, bool enabled = true, bool primary = false) {
            ImGui::BeginDisabled(!enabled);
            if (primary) push_primary_button();
            if (ImGui::Button(label, ImVec2(S(122), 0))) {
                ask = what;
                asked = index;
            }
            if (primary) pop_primary_button();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(0, S(8));
        };
        act("LOAD", Ask::load, current ? "MY MODS already matches this profile"
                                       : "Switch MY MODS to this profile, downloading what is missing", !current, true);
        act("UPDATE", Ask::update, "Overwrite this profile with what MY MODS shows now", !current);
        act("COPY CODE", Ask::code, "Upload this profile to Thunderstore and copy its code");
        act("SAVE FILE", Ask::file, "Save this profile as a .r2z file to send to someone");
        act("RENAME", Ask::rename, "Give this profile another name");
        act("DELETE", Ask::remove, "Delete this profile. No mod is uninstalled.");
        ImGui::NewLine();
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (ask != Ask::none && asked < panel.profiles.list.size()) {
        const auto profile = panel.profiles.list[asked];   // a copy: the list may change under it
        switch (ask) {
        case Ask::load: load_profile(panel, profile); break;
        case Ask::update: {
            panel.profiles.list[asked] = profiles::capture(profile.name, panel.list);
            panel.profiles.active = profile.name;
            if (store(panel)) say(panel, "Updated \"" + profile.name + "\" to what MY MODS shows now.");
            else panel.profiles.list[asked] = profile;
            break;
        }
        case Ask::code: export_code(panel, profile); break;
        case Ask::file: export_file(panel, profile, window); break;
        case Ask::rename:
            panel.profile_rename = profile.name;
            panel.profile_renamed.fill(0);
            std::copy_n(profile.name.begin(), std::min(profile.name.size(), panel.profile_renamed.size() - 1),
                        panel.profile_renamed.begin());
            break;
        case Ask::remove: panel.profile_delete = profile.name; break;
        case Ask::none: break;
        }
    }
}

void profile_popups(ModsPanel& panel) {
    if (!panel.profile_rename.empty() && !ImGui::IsPopupOpen("Rename profile")) ImGui::OpenPopup("Rename profile");
    if (!panel.profile_delete.empty() && !ImGui::IsPopupOpen("Delete profile")) ImGui::OpenPopup("Delete profile");
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Rename profile", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("New name for \"%s\":", panel.profile_rename.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##renamed", panel.profile_renamed.data(), panel.profile_renamed.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        const auto name = trimmed(panel.profile_renamed.data());
        auto* profile = panel.profiles.find(panel.profile_rename);
        const auto* other = panel.profiles.find(name);
        const bool usable = profile && profiles::valid_profile_name(name) && (!other || other == profile);
        if (!usable && !name.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger), "%s",
                other && other != profile ? "Another profile has that name." : "That name cannot be used.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            panel.profile_rename.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!usable);
        push_primary_button();
        if (ImGui::Button("RENAME", ImVec2(S(110), 0)) || (entered && usable)) {
            const auto old = profile->name;
            profile->name = name;
            if (panel.profiles.active == old) panel.profiles.active = name;
            if (store(panel)) say(panel, "Renamed \"" + old + "\" to \"" + name + "\".");
            else {
                profile->name = old;
                if (panel.profiles.active == name) panel.profiles.active = old;
            }
            panel.profile_rename.clear();
            ImGui::CloseCurrentPopup();
        }
        pop_primary_button();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Delete profile", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushTextWrapPos(0);
        ImGui::Text("Delete the profile \"%s\"? Your mods stay installed and stay as they are now.",
            panel.profile_delete.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        // Escape (and a controller's B) answers Cancel, never the destructive choice.
        if (ImGui::Button("Cancel", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            panel.profile_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        push_primary_button();
        if (ImGui::Button("DELETE", ImVec2(S(110), 0))) {
            const auto before = panel.profiles;
            const auto name = panel.profile_delete;
            std::erase_if(panel.profiles.list, [&](const auto& profile) { return profile.name == name; });
            if (panel.profiles.active == name) panel.profiles.active.clear();
            if (store(panel)) say(panel, "Deleted the profile \"" + name + "\". No mod was uninstalled.");
            else panel.profiles = before;
            panel.profile_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        pop_primary_button();
        ImGui::EndPopup();
    }
}

} // namespace dingosdk::launcher_gui::detail
