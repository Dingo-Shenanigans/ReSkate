#include "mod_layers.h"
#include <thread>
#include "Extension/UI/Overlay/overlay.h"

#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_scoring.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/mod_layers.h"
#include "Extension/UI/Startup/startup_window.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace dingosdk {
namespace {
// Mods built for another game version are left out of the merge; say so in a window of its
// own (the game keeps loading behind it) and in the overlay once it draws.
void warn_outdated_mods(const mods::Catalog& catalog) {
    if (catalog.outdated.empty()) return;
    std::string list;
    for (const auto& mod : catalog.outdated) {
        list += "\n  \xE2\x80\xA2 " + (mod.title.empty() ? mod.name : mod.title);
        if (!mod.version.empty()) list += " (v" + mod.version + ")";
    }
    const auto count = catalog.outdated.size();
    const auto text = std::string(count == 1 ? "This mod was" : "These mods were") +
        " built for another version of Skate and " + (count == 1 ? "has" : "have") +
        " been disabled:\n" + list +
        "\n\nThey will not load until they are updated for the current game version. Get an updated "
        "version from the mod's author, or rebuild it with the latest ReSkate Studio.";
    overlay::notify(overlay::NoticeLevel::warning,
        std::to_string(count) + (count == 1 ? " outdated mod disabled" : " outdated mods disabled"),
        "Built for another game version: update " + std::string(count == 1 ? "it" : "them") +
            " to load. See the Mods page or ReSkate.log.");
    std::thread([text] {
        const auto wide = [](const std::string &value) {
            const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
            std::wstring result(static_cast<std::size_t>(std::max(length, 0)), L'\0');
            if (length > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
            return result;
        };
        MessageBoxW(nullptr, wide(text).c_str(), L"ReSkate: outdated mods disabled",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }).detach();
}
// Whether the enabled mods change how tricks score (Engine/Vfs/mod_scoring.h). Multiplayer
// reports it, and hosts keep a player whose tricks score differently out of throwdowns and
// coop challenges. Reading the mods takes a second or two, so it runs beside the game's startup.
void start_scoring_check(const mods::Catalog& catalog) {
    std::thread([&catalog] {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> notes;
        const auto check = mods::check_scoring(catalog, &notes);
        mods::publish_scoring(check);
        for (const auto& note : notes) logging::write(logging::Level::warning, logging::Channel::assets, note);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (!check.fingerprint) {
            logging::log(logging::Level::info, logging::Channel::assets,
                         "Scoring check: tricks score and the skater handles as in the game ({} mod(s) checked in {} ms).", catalog.mods.size(), ms);
            return;
        }
        std::string names, assets;
        for (const auto& mod : check.mods) names += (names.empty() ? "" : ", ") + mod;
        for (std::size_t i = 0; i < check.assets.size() && i < 5; ++i) assets += (assets.empty() ? "" : ", ") + check.assets[i];
        logging::log(logging::Level::warning, logging::Channel::assets,
                     "Scoring check: {} change(s) how tricks score or the skater handles ({} asset(s), e.g. {}; fingerprint {:016x}). In multiplayer, "
                     "dedicated servers and lobby hosts keep you out of throwdowns and coop challenges.",
                     names, check.assets.size(), assets, check.fingerprint);
        overlay::notify(overlay::NoticeLevel::warning,
                        (check.mods.size() == 1 ? check.mods.front() + " changes" : names + " change") + " scoring or physics",
                        "In multiplayer you are kept out of throwdowns and coop challenges while it is enabled.");
    }).detach();
}
namespace fs = std::filesystem;

// `classify` derives a layer kind from the layout.toc path; `load` adds one
// layer, recording the manifest's directory on it and handing the parsed
// manifest to the layout manager.
using ClassifyPath = std::uint8_t (*)(const char*);
using LoadLayer = void (*)(void*, void*, const char*);

namespace layers = addr::mod_layers;
constexpr std::uintptr_t classify_rva = layers::classify;
constexpr std::uintptr_t load_layer_rva = layers::load_layer;
// Slot 0x60 of the layout manager's vtable, which must still be `load`.
constexpr std::uintptr_t load_layer_slot_rva = layers::load_layer_slot;

// The kind the engine gives its own Patch layer. Every mod is merged into one
// patch, so exactly one extra layer exists and no two layers ever share a kind:
// the stock game only runs LCU(3) over Patch(2) over Data(0), and two layers of
// one kind desynchronise bundle reads as soon as content comes from them.
constexpr std::uint8_t patch_layer_kind = 2;
// layout_bootstrap loads Patch only when its manifest exists but always loads
// Data, so the merged layer attaches to whichever of the two arrives first.
constexpr std::string_view patch_prefix = "/native_data/Patch/";
constexpr std::string_view data_prefix = "/native_data/Data/";
constexpr std::string_view mods_prefix = "/native_data/Mods/";

std::atomic<ClassifyPath> original_classify{};
std::atomic<LoadLayer> original_load{};
std::atomic<void*> seen_manager{}, seen_layers{};
std::mutex install_mutex;
std::uintptr_t installed_base{};

struct State {
    fs::path merged_root;
    std::atomic<bool> expanded{};
};
// The hooks and this state outlive startup, like the pinned runtime.
State& state() { static auto* value = new State; return *value; }

bool read_memory(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        destination, size, &read) && read == size;
}

// Supported September 8 build, checked after runtime image validation.
bool validate_contract(std::uintptr_t base) noexcept {
    struct Contract { std::uintptr_t rva; std::span<const std::uint8_t> bytes; };
    const Contract contracts[]{
        {classify_rva, layers::classify_prefix}, {load_layer_rva, layers::load_layer_prefix},
        {layers::layout_bootstrap, layers::layout_bootstrap_prefix}};
    for (const auto& contract : contracts) {
        std::array<std::uint8_t, 32> actual{};
        if (!read_memory(base + contract.rva, actual.data(), contract.bytes.size()) ||
            std::memcmp(actual.data(), contract.bytes.data(), contract.bytes.size())) return false;
    }
    std::uintptr_t slot{};
    return read_memory(base + load_layer_slot_rva, &slot, sizeof(slot)) &&
        slot == base + load_layer_rva;
}

bool starts_with_folded(const char* text, std::string_view prefix) noexcept {
    if (!text) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(text[index]);
        if (!left) return false;
        const auto folded = left >= 'A' && left <= 'Z'
            ? static_cast<unsigned char>(left + ('a' - 'A')) : left;
        const auto right = static_cast<unsigned char>(prefix[index]);
        const auto expected = right >= 'A' && right <= 'Z'
            ? static_cast<unsigned char>(right + ('a' - 'A')) : right;
        if (folded != expected) return false;
    }
    return true;
}

// The engine only asks about the layout.toc it is loading, and the merged patch
// is the one manifest that ever comes from the mods tree.
std::uint8_t classify_path(const char* path) {
    if (starts_with_folded(path, mods_prefix)) return patch_layer_kind;
    return original_classify.load(std::memory_order_acquire)(path);
}

void load_layer(void* self, void* manager, const char* path) {
    const auto original = original_load.load(std::memory_order_acquire);
    auto& value = state();
    const auto stock = starts_with_folded(path, patch_prefix) ? patch_prefix
        : starts_with_folded(path, data_prefix) ? data_prefix : std::string_view{};
    if (stock.empty() || value.expanded.exchange(true)) {
        original(self, manager, path);
        return;
    }
    // -super renames the manifest, so reuse whatever name the engine asked for.
    const std::string file(path + stock.size());
    const auto merged = std::string(mods_prefix) + mods::generated_folder + "/" + file;
    std::error_code error;
    if (fs::is_regular_file(value.merged_root / file, error) && !error) {
        original(self, manager, merged.c_str());
        seen_manager.store(self);
        seen_layers.store(manager);
        logging::log(logging::Level::info, logging::Channel::assets,
            "Merged mod patch mounted: {}", merged);
        // A Patch folder beside the merged layer would be a second layer of the
        // patch kind, which desynchronises bundle reads. Every mod loads from
        // Mods now, so the old Patch is left unmounted.
        if (stock == patch_prefix) {
            logging::log(logging::Level::warning, logging::Channel::assets,
                "Patch folder ignored: mods load from the Mods folder now. Move its contents into a "
                "folder under Mods to keep using them.");
            return;
        }
    } else {
        logging::log(logging::Level::warning, logging::Channel::assets,
            "Merged mod patch {} is missing; no mod content will load.", merged);
    }
    original(self, manager, path);
}

} // namespace

bool start_mod_layers(std::uintptr_t base, std::string& error) {
    std::lock_guard lock(install_mutex);
    error.clear();
    if (installed_base) {
        if (installed_base == base) return true;
        error = "Mod layers are already installed on another image";
        return false;
    }

    // A merge only runs when a mod or the game changed, but then it takes
    // seconds: the startup window shows how far it has got.
    const auto started = std::chrono::steady_clock::now();
    const auto& catalog = mods::catalog([](const mods::MergeProgress& progress) {
        startup::status("Merging " + std::to_string(progress.mods) + (progress.mods == 1 ? " mod" : " mods"),
                        progress.step,
                        progress.total ? static_cast<float>(progress.done) / static_cast<float>(progress.total)
                                       : -1.0f);
    });
    startup::status("Starting ReSkate\xE2\x80\xA6");
    logging::log(logging::Level::info, logging::Channel::assets, "Mods read in {} ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    for (const auto& note : catalog.notes)
        logging::write(logging::Level::info, logging::Channel::assets, note);
    for (const auto& warning : catalog.warnings)
        logging::write(logging::Level::warning, logging::Channel::assets, warning);
    warn_outdated_mods(catalog);
    start_scoring_check(catalog);
    // The same on screen once the overlay draws, so a player who never opens
    // the log still learns which mod is broken before trying to load its map.
    for (const auto& mod : catalog.mods) {
        if (mod.problems.empty()) continue;
        overlay::notify(overlay::NoticeLevel::warning, "Mod " + mod.name + " could not be merged in full",
            std::string(mod.levels.empty() ? "Its content" : "Its map") +
            " may never finish loading. Reinstall the whole mod folder, or rebuild it with a current "
            "ReSkate Studio. Details are in ReSkate.log.");
    }
    // A mod left out of the merge altogether loads none of its content, which
    // is worse, and used to say so only in the log. Name those too; the
    // outdated ones have their own notice above.
    {
        std::vector<const mods::Mod*> dropped;
        for (const auto& mod : catalog.excluded)
            if (mod.outdated.empty()) dropped.push_back(&mod);
        if (!dropped.empty()) {
            std::string names;
            for (const auto* mod : dropped)
                names += (names.empty() ? "" : ", ") + (mod->title.empty() ? mod->name : mod->title);
            const bool one = dropped.size() == 1;
            overlay::notify(overlay::NoticeLevel::warning,
                one ? "A mod could not be merged"
                    : std::to_string(dropped.size()) + " mods could not be merged",
                names + (one ? " was" : " were") + " left out, so none of " + (one ? "its" : "their") +
                " content is loaded. Reinstall the whole mod folder, or rebuild it with a current "
                "ReSkate Studio. Details are in ReSkate.log.");
        }
    }
    // A rejected catalogue is player-authored content, not a broken contract:
    // report it and boot with the stock layers rather than failing startup.
    if (!catalog.issue.empty()) {
        logging::log(logging::Level::error, logging::Channel::assets,
            "Mods folder ignored: {}.", catalog.issue);
        overlay::notify(overlay::NoticeLevel::error, "Mods folder ignored",
            catalog.issue + ". No mod is loaded. Details are in ReSkate.log.");
        return true;
    }
    if (!catalog.present) {
        logging::log(logging::Level::info, logging::Channel::assets,
            "No Mods folder at {}; only Patch and Data are layered.", path_utf8(catalog.root));
        return true;
    }
    if (!catalog.merged) {
        logging::write(logging::Level::info, logging::Channel::assets,
            "No mod provides a layout.toc; no merged patch was built.");
        return true;
    }
    if (!validate_contract(base)) {
        error = "Native layout-layer contract does not match the supported game";
        return false;
    }

    state().merged_root = catalog.root / mods::generated_folder;

    auto* classify_target = reinterpret_cast<void*>(base + classify_rva);
    auto* load_target = reinterpret_cast<void*>(base + load_layer_rva);
    ClassifyPath classify_original{};
    LoadLayer load_original{};
    auto created = hook_prepare(classify_target, reinterpret_cast<void*>(&classify_path),
        reinterpret_cast<void**>(&classify_original));
    if (created == HookOk && !classify_original) { hook_remove(classify_target); created = HookNotFound; }
    if (created != HookOk) {
        error = "Cannot create the layer classifier hook (Detours hook service status " +
            std::to_string(created) + ")";
        return false;
    }
    auto load_created = hook_prepare(load_target, reinterpret_cast<void*>(&load_layer),
        reinterpret_cast<void**>(&load_original));
    if (load_created == HookOk && !load_original) { hook_remove(load_target); load_created = HookNotFound; }
    if (load_created != HookOk) {
        hook_remove(classify_target);
        error = "Cannot create the layout loader hook (Detours hook service status " +
            std::to_string(load_created) + ")";
        return false;
    }
    original_classify.store(classify_original, std::memory_order_release);
    original_load.store(load_original, std::memory_order_release);

    // Both hooks become visible in one Detours transaction before game startup.
    auto enabled = hook_queue_enable(classify_target);
    if (enabled == HookOk) enabled = hook_queue_enable(load_target);
    if (enabled == HookOk) enabled = hook_apply_queued();
    if (enabled != HookOk) {
        hook_remove(load_target);
        hook_remove(classify_target);
        original_load.store(nullptr, std::memory_order_release);
        original_classify.store(nullptr, std::memory_order_release);
        error = "Cannot enable the merged mod patch (Detours hook service status " +
            std::to_string(enabled) + ")";
        return false;
    }
    installed_base = base;
    logging::log(logging::Level::info, logging::Channel::assets,
        "Merged mod patch ready at {}", path_utf8(state().merged_root));
    return true;
}
LayoutObjects layout_objects() noexcept { return {seen_manager.load(), seen_layers.load()}; }
}
