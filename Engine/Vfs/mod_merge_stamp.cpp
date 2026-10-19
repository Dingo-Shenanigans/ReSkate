#include "mod_merge_internal.h"
#include "Engine/Core/Platform/path_text.h"

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
// -- Keeping the patch between launches ------------------------------------
//
// A merge reads the SDK doing it, every enabled mod's files in priority order,
// and the base files they are merged onto. When none of those changed, the
// patch the last launch built is the patch this one would build, so it is kept.
// A stamp written last, once the patch is complete, records what it was built
// from and every file it holds.

constexpr const wchar_t* stamp_name = stamp_file;
constexpr char stamp_header[] = "ReSkate merge 1";

std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

// The module this code runs in: rebuilding the SDK can change how it merges.
fs::path sdk_module() {
    // ReSkate.dll beside the running executable, never the module this code
    // happens to live in. Skate.exe and ReSkateLauncher.exe sit beside it and
    // both merge; naming different files would mean neither ever reuses the
    // other's merge, and the work would be done twice on every launch.
    std::wstring host(32768, wchar_t{});
    const auto length = GetModuleFileNameW(nullptr, host.data(), static_cast<DWORD>(host.size()));
    if (!length || length >= host.size()) return {};
    host.resize(length);
    return fs::path(host).parent_path() / L"ReSkate.dll";
}

} // namespace

std::string merge_fingerprint(const Catalog& catalog, const std::vector<const Mod*>& mods,
                              const std::map<const Mod*, RelativeFiles>& modFiles, bool storeKnown) {
    std::string inputs = stamp_header;
    inputs += '\n';
    inputs += storeKnown ? "store known\n" : "store unknown\n";
    const auto describe = [&](std::string_view label, const fs::path& path) {
        inputs += label;
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error) { inputs += " missing\n"; return; }
        const auto written = fs::last_write_time(path, error);
        inputs += ' ' + std::to_string(size) + ' ' +
                  std::to_string(error ? 0 : written.time_since_epoch().count()) + '\n';
    };
    describe("sdk", sdk_module());
    const auto baseRoot = catalog.data_root / L"Data";
    describe("base layout.toc", baseRoot / L"layout.toc");
    std::set<std::string> tocs;
    for (const auto* mod : mods) {
        inputs += "mod " + mod->name + '\n';
        std::vector<fs::path> files;
        std::error_code error;
        for (fs::recursive_directory_iterator it(mod->directory, error), end; it != end && !error;
             it.increment(error)) {
            if (it->is_regular_file(error) && !error) files.push_back(it->path());
            error.clear();
        }
        if (error) throw std::runtime_error("Cannot list " + path_utf8(mod->directory));
        std::ranges::sort(files);
        for (const auto& file : files)
            describe(utf8(fs::relative(file, mod->directory, error)), file);
        for (const auto& toc : modFiles.at(mod).tocs) tocs.insert(lower(toc));
    }
    for (const auto& toc : tocs) {
        auto base = baseRoot / from_utf8(toc);
        describe("base " + toc, base);
        describe("base sb", base.replace_extension(L".sb"));
    }
    const auto digest = sha1_of(std::as_bytes(std::span(inputs)));
    std::string hex;
    for (const auto value : digest.bytes) {
        constexpr char digits[] = "0123456789abcdef";
        hex += digits[static_cast<unsigned>(value) >> 4];
        hex += digits[static_cast<unsigned>(value) & 15];
    }
    return hex;
}

// The previous build, when it was made from these inputs and still holds
// every file it was published with.
std::optional<MergeReport> previous_merge(const fs::path& output, const std::string& fingerprint) {
    try {
        std::ifstream input(output / stamp_name, std::ios::binary);
        std::string line;
        if (!input || !std::getline(input, line) || line != stamp_header) return std::nullopt;
        MergeReport report;
        bool matched{};
        std::size_t files{};
        while (std::getline(input, line)) {
            const auto space = line.find(' ');
            if (space == std::string::npos) continue;
            const auto key = std::string_view(line).substr(0, space);
            const auto value = std::string_view(line).substr(space + 1);
            const auto count = [&] { return static_cast<std::size_t>(std::stoull(std::string(value))); };
            if (key == "fingerprint") {
                if (value != fingerprint) return std::nullopt;
                matched = true;
            } else if (key == "superbundles") {
                report.superbundles = count();
            } else if (key == "archives") {
                report.archives = count();
            } else if (key == "bundles") {
                report.mergedBundles = count();
            } else if (key == "assets") {
                report.mergedAssets = count();
            } else if (key == "file") {
                const auto gap = value.find(' ');
                if (gap == std::string_view::npos) return std::nullopt;
                std::error_code error;
                const auto size = fs::file_size(output / from_utf8(value.substr(gap + 1)), error);
                if (error || size != std::stoull(std::string(value.substr(0, gap)))) return std::nullopt;
                ++files;
            } else if (key == "note") {
                report.notes.emplace_back(value);
            } else if (key == "problem") {
                const auto bar = value.find('|');
                if (bar != std::string_view::npos)
                    report.problems[std::string(value.substr(0, bar))].emplace_back(value.substr(bar + 1));
            }
        }
        if (!matched || !files) return std::nullopt;
        report.built = true;
        report.reused = true;
        return report;
    } catch (...) {
        return std::nullopt;  // an unreadable stamp just means building again
    }
}

void write_stamp(const fs::path& output, const std::string& fingerprint, const MergeReport& report) {
    std::string text = std::string(stamp_header) + "\nfingerprint " + fingerprint + '\n';
    text += "superbundles " + std::to_string(report.superbundles) + '\n';
    text += "archives " + std::to_string(report.archives) + '\n';
    text += "bundles " + std::to_string(report.mergedBundles) + '\n';
    text += "assets " + std::to_string(report.mergedAssets) + '\n';
    std::error_code error;
    for (fs::recursive_directory_iterator it(output, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || error) { error.clear(); continue; }
        const auto size = fs::file_size(it->path(), error);
        if (error) throw std::runtime_error("Cannot size " + path_utf8(it->path()));
        text += "file " + std::to_string(size) + ' ' + utf8(fs::relative(it->path(), output, error)) + '\n';
    }
    if (error) throw std::runtime_error("Cannot list " + path_utf8(output));
    for (auto note : report.notes) {
        std::ranges::replace(note, '\n', ' ');
        std::ranges::replace(note, '\r', ' ');
        text += "note " + note + '\n';
    }
    // Mod folder names never contain '|', so it separates the mod from the text.
    for (const auto& [mod, problems] : report.problems) {
        for (auto problem : problems) {
            std::ranges::replace(problem, '\n', ' ');
            std::ranges::replace(problem, '\r', ' ');
            text += "problem " + mod + '|' + problem + '\n';
        }
    }
    write_file(output / stamp_name, std::as_bytes(std::span(text)));
}

} // namespace dingosdk::mods::detail
