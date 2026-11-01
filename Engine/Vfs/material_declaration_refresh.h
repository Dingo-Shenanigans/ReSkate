#pragma once
#include "mod_materials.h"
#include "mod_item_sources.h"
#include <memory>
#include <utility>

namespace dingosdk::mods {
struct MaterialDeclarations {
    std::uint64_t revision{};
    std::vector<MaterialDeclaration> materials;
    std::vector<ItemSource> sources;
};
struct DeclarationFileStamp {
    std::filesystem::path path;
    std::string owner;
    bool present{};
    std::uintmax_t size{};
    std::filesystem::file_time_type written{};
    std::uint64_t revision{};
    bool operator==(const DeclarationFileStamp&) const = default;
};
inline DeclarationFileStamp declaration_file_stamp(const std::filesystem::path& path, std::string owner = {}) {
    DeclarationFileStamp stamp{path, std::move(owner)};
    stamp.present = std::filesystem::exists(path);
    if (stamp.present) {
        stamp.size = std::filesystem::file_size(path);
        stamp.written = std::filesystem::last_write_time(path);
    }
    return stamp;
}
// Owned by the single refresh worker. No successful signature or value changes
// until a complete load succeeds, so failed/transient reads can be retried.
class MaterialDeclarationRefresh {
public:
    template<class Load> bool update(const std::vector<DeclarationFileStamp>& signature, Load&& load) {
        if (value_ && signature == signature_) return false;
        auto next = std::make_shared<const MaterialDeclarations>(load());
        auto next_signature = signature;
        signature_ = std::move(next_signature);
        value_ = std::move(next);
        return true;
    }
    std::shared_ptr<const MaterialDeclarations> value() const { return value_; }
private:
    std::vector<DeclarationFileStamp> signature_;
    std::shared_ptr<const MaterialDeclarations> value_;
};
}
