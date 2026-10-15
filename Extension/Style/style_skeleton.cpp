#include "style_skeleton.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <stdexcept>

namespace dingosdk::style {
frostbite::Skeleton read_game_skeleton(const std::filesystem::path &game_root) {
    const vfs::GameData data(game_root);
    const auto bundle = data.read_bundle(data.read_toc(addr::style::skeleton_toc), addr::style::skeleton_bundle);
    if (!bundle) throw std::runtime_error("the game's configuration bundle was not found");
    std::size_t index{};
    const auto *payload = bundle->find(frostbite::AssetKind::ebx, addr::style::skeleton_asset, &index)
                              ? bundle->payload(frostbite::AssetKind::ebx, index)
                              : nullptr;
    if (!payload) throw std::runtime_error("the skater skeleton is not in the configuration bundle");
    const auto document = frostbite::ebx::read_document(data.read(*payload));
    const auto *root = document.root();
    if (!root || !root->object || document.rootType != "AntSkeletonAsset")
        throw std::runtime_error("the skater skeleton is not the AntSkeletonAsset this build expects");
    auto skeleton = frostbite::read_ant_skeleton(*root->object);
    if (skeleton.names.size() != addr::style::skeleton_joints) throw std::runtime_error("the skater skeleton has another joint count");
    return skeleton;
}
} // namespace dingosdk::style
