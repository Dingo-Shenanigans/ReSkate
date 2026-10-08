#include "style_skeleton.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace dingosdk::style {
namespace {
namespace fb = frostbite;
namespace ebx = frostbite::ebx;
constexpr std::string_view bundle_name = "win32/configurations/gameconfigurations/delmargameconfiguration";
constexpr std::string_view asset_name = "animation/dingo/animbase_default_skeleton";

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
std::int64_t integer(const ebx::Value &value) {
    if (const auto *i = std::get_if<std::int64_t>(&value.data)) return *i;
    if (const auto *u = std::get_if<std::uint64_t>(&value.data)) return static_cast<std::int64_t>(*u);
    throw std::runtime_error("a skeleton value is not a number");
}
const ebx::Value::Array &array(const ebx::Object &object, std::string_view field) {
    const auto *found = object.find(field);
    const auto *values = found ? std::get_if<ebx::Value::Array>(&found->value.data) : nullptr;
    if (!values) throw std::runtime_error("the skeleton has no " + std::string(field));
    return *values;
}
} // namespace

std::vector<SkeletonJoint> read_game_skeleton(const std::filesystem::path &game_root) {
    const vfs::GameData data(game_root);
    std::optional<vfs::GameBundle> bundle;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(game_root / L"Data" / L"Win32", error), end;
         it != end && !error && !bundle; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".toc") continue;
        try {
            bundle = data.read_bundle(data.read_toc(std::filesystem::relative(it->path(), game_root / L"Data").generic_string()),
                                      bundle_name);
        } catch (const std::exception &) {}
    }
    if (!bundle) throw std::runtime_error("the game's configuration bundle was not found");
    std::size_t index = bundle->manifest.ebx.size();
    for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i)
        if (lower(bundle->manifest.ebx[i].name) == asset_name) { index = i; break; }
    const auto *payload = bundle->payload(fb::AssetKind::ebx, index);
    if (!payload) throw std::runtime_error("the skater skeleton is not in the configuration bundle");
    const auto document = ebx::read_document(data.read(*payload));
    const auto *root = document.root();
    if (!root || !root->object || document.rootType != "AntSkeletonAsset")
        throw std::runtime_error("the skater skeleton is not the AntSkeletonAsset this build expects");
    const auto &joints = array(*root->object, "Joints");
    const auto &table = array(*root->object, "NameTable");
    if (joints.size() != addr::style::skeleton_joints) throw std::runtime_error("the skater skeleton has another joint count");
    std::string names;
    names.reserve(table.size());
    for (const auto &value : table) names.push_back(static_cast<char>(integer(value)));
    std::vector<SkeletonJoint> result;
    result.reserve(joints.size());
    for (const auto &value : joints) {
        const auto *joint = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
        const auto *name = joint && *joint ? (*joint)->find("Name") : nullptr;
        const auto *parent = name ? (*joint)->find("Parent") : nullptr;
        if (!parent) throw std::runtime_error("a skeleton joint is incomplete");
        const auto start = integer(name->value);
        if (start < 0 || static_cast<std::size_t>(start) >= names.size()) throw std::runtime_error("a skeleton joint name is misplaced");
        const auto stop = names.find('\0', static_cast<std::size_t>(start));
        if (stop == std::string::npos) throw std::runtime_error("a skeleton joint name is cut short");
        const auto above = integer(parent->value);
        if (above < -1 || above >= static_cast<std::int64_t>(result.size())) throw std::runtime_error("a skeleton joint's parent is out of order");
        result.push_back({names.substr(static_cast<std::size_t>(start), stop - static_cast<std::size_t>(start)),
                          static_cast<std::int32_t>(above)});
    }
    return result;
}
} // namespace dingosdk::style
