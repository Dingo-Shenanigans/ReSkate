#pragma once
#include "skate_rules.h"
#include <array>
#include <deque>
#include <mutex>
namespace dingosdk::skate {
struct DetectedAttempt {
    std::uint64_t id{};
    bool landed{};
    Category violation{Category::none};
    std::vector<std::string> signature;
    std::string display;
    Json evidence{Json::object()};
    std::array<std::uint8_t,28> native{};
};
bool active() noexcept;
void bridge_error() noexcept;
bool take_bridge_error() noexcept;
void begin(Rules rules);
void end();
void turn(bool own);
void observe(const Json& row);
void sample(trick_logger::SetSample sample);
// Native client filtering is widened only for detector-controlled turns. The
// game's own area/sequence code remains the sole producer of eligibility.
std::optional<bool> native_ignore_observation(std::string_view scorable_name);
std::optional<bool> native_category_permission(std::uint32_t category);
void reset_native_eligibility();
void physics_transition(std::uint32_t before,std::uint32_t after,std::uint64_t now);
void native_outcome(bool landed,const std::array<std::uint8_t,28>& record);
std::vector<std::string> take_diagnostics();
std::optional<DetectedAttempt> take();
// The native handler supplies setter/follower ownership, rather than inferring it
// from packet arrival or the display. Called on its server expression thread.
void prepare_native(std::uint32_t player,DetectedAttempt attempt);
bool native_pending(std::uint32_t player);
bool judge_native(std::uint32_t player,bool setter,std::array<std::uint8_t,28>& record,bool& landed);
std::string target_text();
}
