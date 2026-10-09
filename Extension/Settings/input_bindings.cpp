#include "Extension/Profile/profile_internal.h"
#include <cmath>
#include <set>
#include <type_traits>

namespace dingosdk::profile {
using namespace detail;
std::uint32_t freecam_controller_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("freecam_controller");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Freecam Controller binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in Freecam Controller binding");
    return static_cast<std::uint32_t>(mask);
}
bool freecam_controller(const Snapshot& s) {
    const auto options = s.settings.find("options");
    if (options == s.settings.end()) return false;
    require(options->is_object(), "Options must be an object");
    const auto value = options->find("freecam_controller");
    if (value == options->end()) return false;
    require(value->is_boolean(), "freecam_controller must be a boolean");
    return value->get<bool>();
}
std::uint32_t freecam_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("freecam");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Freecam binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in Freecam binding");
    return static_cast<std::uint32_t>(mask);
}
std::uint32_t tp_to_freecam_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("tp_to_freecam");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "TP to Freecam binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in TP to Freecam binding");
    return static_cast<std::uint32_t>(mask);
}
std::uint32_t noclip_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("noclip");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Noclip binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in Noclip binding");
    return static_cast<std::uint32_t>(mask);
}

std::uint32_t forward_velocity_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("forward_velocity");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Forward velocity binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in Forward Boost binding");
    return static_cast<std::uint32_t>(mask);
}

std::uint32_t offboard_up_velocity_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("offboard_up_velocity");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Off-board Up Boost binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in Off-board Up Boost binding");
    return static_cast<std::uint32_t>(mask);
}
std::uint32_t up_velocity_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("up_velocity");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Up velocity binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= UINT32_MAX && valid_action_binding(static_cast<std::uint32_t>(mask)),
        "Unsupported controller or keyboard key in Up Boost binding");
    return static_cast<std::uint32_t>(mask);
}

}
