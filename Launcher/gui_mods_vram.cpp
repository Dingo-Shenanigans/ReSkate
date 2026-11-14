#include "gui_internal.h"

#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_vram.h"

#include "Engine/Core/Log/logging.h"

#include <algorithm>
#include <fstream>

// MY MODS: how much video memory each mod can take, said on its row. A map only costs while it is
// loaded; game data in the bundles the game keeps loaded costs from the start. An item's look costs
// only while a skater wears it, and nobody wears a whole pack, so that is said in a mod's detail and
// is not in its figure. Each mod's figures are read from its files on a worker
// (Engine/Vfs/mod_vram.h) and kept in Mods/.reskate-vram.txt by the mod's fingerprint, so only a
// mod that changed is read again. An estimate of what the mod ships, not of what Skate takes.
namespace dingosdk::launcher_gui::detail {
namespace {

constexpr wchar_t cache_file[] = L".reskate-vram.txt";

struct Job {
    std::string name;
    fs::path directory;               // the mod's folder: what its fingerprint is of
    std::vector<fs::path> own;        // the folders always in the game (the mod's own, when it ships a layout)
    std::vector<fs::path> variants;   // its variants' folders: one of them is in the game at a time
};

void add(mods::ModVram& to, const mods::ModVram& more) {
    // (A mod's own levels and a variant's are not loaded together; neither are two mods'.)
    if (more.level > to.level) {
        to.level = more.level;
        to.level_resident = more.level_resident;
    }
    to.always += more.always;
    to.on_demand += more.on_demand;
    to.on_demand_largest = std::max(to.on_demand_largest, more.on_demand_largest);
    to.on_demand_bundles += more.on_demand_bundles;
    to.textures += more.textures;
    to.meshes += more.meshes;
    to.unread += more.unread;
    if (to.problem.empty()) to.problem = more.problem;
}

mods::ModVram read(const Job& job, const fs::path& data_root, const std::atomic<bool>& stop) {
    mods::ModVram all;
    for (const auto& folder : job.own) add(all, mods::read_mod_vram(data_root, folder, &stop));
    // Of the variants, the heaviest: whichever is chosen, the mod takes no more.
    mods::ModVram heaviest;
    for (const auto& folder : job.variants) {
        auto one = mods::read_mod_vram(data_root, folder, &stop);
        if (!one.problem.empty() && all.problem.empty()) all.problem = one.problem;
        if (one.most() >= heaviest.most()) heaviest = std::move(one);
    }
    add(all, heaviest);
    return all;
}

// The figures kept from before: "<folder>\t<fingerprint>\t<figures>" a line.
using Kept = std::map<std::string, std::pair<std::string, mods::ModVram>, std::less<>>;
Kept read_kept(const fs::path& file) {
    Kept kept;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (second == std::string::npos) continue;
        mods::ModVram vram;
        if (mods::parse_vram_line(std::string_view(line).substr(second + 1), vram))
            kept[line.substr(0, first)] = {line.substr(first + 1, second - first - 1), vram};
    }
    return kept;
}
void write_kept(const fs::path& file, const Kept& kept) {
    std::ofstream out(file, std::ios::trunc);
    for (const auto& [name, entry] : kept) out << name << '\t' << entry.first << '\t' << mods::vram_line(entry.second) << '\n';
}

void work(ModVramScan& scan, std::vector<Job> jobs, fs::path root) {
    const auto data_root = root.parent_path();
    const auto file = root / cache_file;
    const auto before = read_kept(file);
    Kept kept;
    bool changed = before.size() != jobs.size();
    for (const auto& job : jobs) {
        if (scan.stop) break;
        mods::ModVram vram;
        try {
            const auto fingerprint = mods::mod_fingerprint(job.directory);
            if (const auto found = before.find(job.name); found != before.end() && found->second.first == fingerprint) {
                vram = found->second.second;
                kept[job.name] = found->second;
            } else {
                vram = read(job, data_root, scan.stop);
                if (scan.stop) break;   // (cut short: not what the mod costs)
                // One that could not be read is read again next time, and says so in the log once a run.
                if (vram.problem.empty()) kept[job.name] = {fingerprint, vram};
                else logging::log(logging::Level::warning, logging::Channel::launcher, "{}: its video memory could not be read: {}",
                                  job.name, vram.problem);
                changed = true;
            }
        } catch (const std::exception& failure) {
            vram.problem = failure.what();
        }
        std::lock_guard lock(scan.mutex);
        scan.done.emplace_back(job.name, std::move(vram));
    }
    if (!scan.stop && changed) write_kept(file, kept);
    scan.running = false;
}

// What a mod takes whatever anyone wears: its map while that is loaded, and what is always loaded.
std::uint64_t standing(const mods::ModVram& vram) { return vram.level + vram.always; }

} // namespace

void start_vram_scan(ModsPanel& panel) {
    auto& scan = panel.vram;
    scan.stop = true;
    if (scan.worker.joinable()) scan.worker.join();
    scan.stop = false;
    scan.interrupted = false;
    std::vector<Job> jobs;
    for (const auto& entry : panel.list.entries) {
        const auto& mod = entry.mod;
        if (!mod.provides_layout && !mod.variants) continue;
        Job job{mod.name, mod.directory, {}, {}};
        if (mod.provides_layout) job.own.push_back(mod.directory);
        if (mod.variants)
            for (const auto& variant : mods::installed_variants(mod))
                if (auto folder = mods::variant_folder(mod, variant); !folder.empty()) job.variants.push_back(std::move(folder));
        jobs.push_back(std::move(job));
    }
    {
        std::lock_guard lock(scan.mutex);
        scan.done.clear();
    }
    // A mod that is gone is forgotten; the others keep what they showed until they are read again.
    std::erase_if(scan.known, [&](const auto& known) {
        return std::none_of(jobs.begin(), jobs.end(), [&](const Job& job) { return job.name == known.first; });
    });
    scan.waiting = jobs.size();
    if (jobs.empty()) return;
    scan.running = true;
    scan.worker = std::thread(work, std::ref(scan), std::move(jobs), panel.root);
}

void pump_vram_scan(ModsPanel& panel) {
    auto& scan = panel.vram;
    {
        std::lock_guard lock(scan.mutex);
        for (auto& [name, vram] : scan.done) {
            scan.known[name] = std::move(vram);
            if (scan.waiting) --scan.waiting;
        }
        scan.done.clear();
    }
    // Stopped for an install or an uninstall: read again once the folder is still.
    if (scan.interrupted && !scan.running && !panel.installing) start_vram_scan(panel);
}

void wait_for_vram_scan(ModsPanel& panel) {
    auto& scan = panel.vram;
    if (!scan.running) return;
    scan.stop = true;
    scan.interrupted = true;
    for (int waited = 0; scan.running && waited < 500; ++waited) Sleep(20);
}

const mods::ModVram* mod_vram(const ModsPanel& panel, std::string_view name) {
    const auto found = panel.vram.known.find(name);
    return found == panel.vram.known.end() || !found->second.problem.empty() ? nullptr : &found->second;
}

std::string mod_vram_text(const ModsPanel& panel, const mods::Mod& mod) {
    const auto* vram = mod_vram(panel, mod.name);
    return vram && standing(*vram) ? size_text(standing(*vram)) : std::string();
}

std::string mod_vram_detail(const ModsPanel& panel, const mods::Mod& mod) {
    const auto* vram = mod_vram(panel, mod.name);
    if (!vram || !vram->most()) return {};
    std::string text;
    const auto line = [&](const std::string& more) { text += (text.empty() ? "" : "\n") + more; };
    if (vram->level) line("Up to " + size_text(vram->level) + " while its map is loaded");
    if (vram->always) line(size_text(vram->always) + " always loaded");
    if (vram->on_demand)
        line("Not counted: its items' looks, loaded while worn (" + size_text(vram->on_demand) + " for all of them)");
    return text;
}

} // namespace dingosdk::launcher_gui::detail
