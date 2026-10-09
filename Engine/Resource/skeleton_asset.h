#pragma once
// Frostbite SkeletonAssets: a skeleton's bones and how they hang together.
#include "ebx_document.h"
#include <string>
#include <vector>

namespace dingosdk::frostbite {
struct Skeleton {
    std::vector<std::string> names;
    std::vector<int> parents; // each bone's, before it; -1 for the root
};
// `asset` is the SkeletonAsset (an EBX document's root object). Throws when it is not one.
[[nodiscard]] Skeleton read_skeleton(const ebx::Object& asset);
}
