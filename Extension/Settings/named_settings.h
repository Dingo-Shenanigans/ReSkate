#pragma once
#include "Engine/Game/Settings/named_settings.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace dingosdk {
// Initialize before installing the client update hook. All other functions
// are owned by that verified game thread; Present receives copied models.
bool initialize_named_settings(std::uintptr_t base);
// `wanted`: a menu or console is showing the values, so refresh them quickly.
// Otherwise they are refreshed a few at a time in the background.
void refresh_named_settings(bool wanted = true);
const std::vector<NamedSettingModel> &named_settings_model();
// Increases whenever any row of named_settings_model() changes.
std::uint64_t named_settings_revision();
std::string change_named_setting(std::string_view name, std::string_view value, bool restore);
// The setting's value now, as the console shows it; nothing when it cannot be read.
std::optional<std::string> named_setting_value(std::string_view name);
// The same for a player's own change (the console): one that changes how the game plays is
// refused during a multiplayer session (Engine/Game/Settings/multiplayer_settings_lock.h), and
// any such change made before a session is put back when it starts.
std::string player_change_named_setting(std::string_view name, std::string_view value, bool restore);
std::string restore_named_settings();
// Every engine setting name known so far: listed from the engine's settings
// registry this session or remembered from earlier ones. Any thread.
std::vector<std::string> named_setting_names();
// True while `current` is the number `held` in any form, such as "60" and "60.0".
[[nodiscard]] bool still_held(std::string_view current, std::string_view held) noexcept;
// Named settings that a feature sets for a while. Each is put back only while it still holds the value the feature set.
class HeldSettings {
public:
    using Reader = std::optional<std::string> (*)(std::string_view name);
    // The answer starts with "error" when the setting was not set.
    using Writer = std::string (*)(std::string_view name, std::string_view value);
    HeldSettings(Reader read, Writer write) noexcept;
    // False at the first setting that fails, with `error` as a lowercase fragment.
    bool hold(std::span<const std::pair<std::string_view, std::string_view>> settings, std::string &error);
    // Returns and logs the settings it could not read or put back.
    std::vector<std::string> release() noexcept;

private:
    struct Held {
        std::string name, before, value;
    };
    [[nodiscard]] bool put_back(const Held &setting) const noexcept;
    Reader read_;
    Writer write_;
    std::vector<Held> held_;
};
} // namespace dingosdk
