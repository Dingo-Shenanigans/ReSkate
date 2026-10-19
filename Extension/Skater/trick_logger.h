#pragma once
#include <array>
#include "trick_diagnostics.h"
#include <cstdint>
#include <string>
#include <string_view>
namespace dingosdk::multiplayer { struct NativeFrame; }
namespace dingosdk { class Json; }
namespace dingosdk::trick_logger {
bool enabled() noexcept;
bool full_capture() noexcept;
bool gameplay_capture() noexcept;
std::uint64_t generation() noexcept;
bool named_trick(Json value) noexcept;
void name_status(std::string text) noexcept;
std::string command(std::string_view arguments);
std::string feedback_text() noexcept;
// Bits follow the seven active native setup rows, not the outgoing wire array.
struct SetupFlags {std::uint8_t captured{},valid{};};
SetupFlags setup_flags() noexcept;
void observe_setup_menu(std::uint8_t mask, std::uint8_t valid) noexcept;
void sample(const multiplayer::NativeFrame& frame, std::string_view map) noexcept;
void physics_transition(std::uint32_t before, std::uint32_t after) noexcept;
void physics_step(std::uintptr_t entity, float seconds, bool wipeout) noexcept;
void native_capture_trace(bool ignored,bool banned,bool allowed,std::uint32_t first_valid) noexcept;
void native_observation(std::string_view name,bool original,bool selected) noexcept;
void native_filter(std::uint32_t category,bool original,bool selected) noexcept;
void scoring_expression(std::uintptr_t vm, std::uint32_t pc, bool after) noexcept;
void gameplay_bool(std::string_view key, bool value, std::string_view source) noexcept;
void setup_snapshot(Json value) noexcept;
void detected_attempt(Json value) noexcept;
void event(std::uint32_t hash) noexcept;
void attempt(bool landed, const std::array<std::uint8_t, 28>& record) noexcept;
void score(std::uint32_t player, bool add, float value, std::uint32_t graph) noexcept;
}
