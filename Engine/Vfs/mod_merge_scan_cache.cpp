#include "mod_merge_internal.h"

#include <Windows.h>

#include <fstream>
#include <sstream>

namespace dingosdk::mods::detail {
namespace {
// The file's first line: its format, and the ReSkate that wrote it (sdk_stamp). Another ReSkate
// may read a mod differently, so what this one found is not taken on.
constexpr std::string_view scan_header = "ReSkate scan 1";

std::vector<std::uint32_t> numbers(const std::string& text) {
    std::vector<std::uint32_t> list;
    std::istringstream in(text);
    for (std::uint32_t value{}; in >> value;) list.push_back(value);
    return list;
}
} // namespace

ScanCache::ScanCache(fs::path file, std::map<std::string, std::string, std::less<>> fingerprints) noexcept
    : file_(std::move(file)), fingerprints_(std::move(fingerprints)) {
    try {
        std::ifstream in(file_, std::ios::binary);
        std::string line;
        if (!in || !std::getline(in, line)) return;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != std::string(scan_header) + ' ' + sdk_stamp()) return;
        std::map<std::string, Bundle, std::less<>>* mod{};
        Bundle* bundle{};
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const auto kind = line.substr(0, tab);
            const auto rest = line.substr(tab + 1);
            if (kind == "mod") {
                // A mod's results count while its files are as they were (its fingerprint).
                const auto split = rest.find('\t');
                const auto name = rest.substr(0, split);
                const auto known = fingerprints_.find(name);
                mod = split != std::string::npos && known != fingerprints_.end() && known->second == rest.substr(split + 1)
                    ? &known_[name] : nullptr;
                bundle = nullptr;
            } else if (kind == "b") {
                bundle = mod ? &(*mod)[rest] : nullptr;
            } else if (bundle && kind == "r") {
                bundle->repair = numbers(rest);
            } else if (bundle && kind == "t") {
                bundle->textures = numbers(rest);
            } else if (bundle && kind == "m") {
                std::istringstream values(rest);
                for (Mesh mesh; values >> mesh.chunk >> mesh.archive >> mesh.offset >> mesh.bytes;) bundle->meshes.push_back(mesh);
            }
        }
    } catch (...) {
        known_.clear();   // an unreadable file is as good as none
    }
}

std::optional<ScanCache::Bundle> ScanCache::find(const std::string& mod, const std::string& key) const {
    std::lock_guard lock(mutex_);
    const auto known = known_.find(mod);
    if (known == known_.end()) return std::nullopt;
    const auto bundle = known->second.find(key);
    if (bundle == known->second.end()) return std::nullopt;
    return bundle->second;
}

void ScanCache::keep(const std::string& mod, const std::string& key, Bundle bundle) {
    if (!fingerprints_.contains(mod)) return;
    std::lock_guard lock(mutex_);
    known_[mod][key] = std::move(bundle);
}

void ScanCache::save() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        const auto temporary = fs::path(file_).concat(L".new");
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << scan_header << ' ' << sdk_stamp() << '\n';
            const auto list = [&](const std::vector<std::uint32_t>& values) {
                std::string text;
                for (const auto value : values) text += (text.empty() ? "" : " ") + std::to_string(value);
                return text;
            };
            for (const auto& [name, bundles] : known_) {
                const auto fingerprint = fingerprints_.find(name);
                if (fingerprint == fingerprints_.end()) continue;
                out << "mod\t" << name << '\t' << fingerprint->second << '\n';
                for (const auto& [key, bundle] : bundles) {
                    out << "b\t" << key << '\n';
                    if (!bundle.repair.empty()) out << "r\t" << list(bundle.repair) << '\n';
                    if (!bundle.textures.empty()) out << "t\t" << list(bundle.textures) << '\n';
                    if (!bundle.meshes.empty()) {
                        out << "m\t";
                        for (std::size_t at = 0; at < bundle.meshes.size(); ++at) {
                            const auto& mesh = bundle.meshes[at];
                            out << (at ? " " : "") << mesh.chunk << ' ' << mesh.archive << ' ' << mesh.offset << ' ' << mesh.bytes;
                        }
                        out << '\n';
                    }
                }
            }
            if (!out.flush()) return;
        }
        MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } catch (...) {}
}

} // namespace dingosdk::mods::detail
