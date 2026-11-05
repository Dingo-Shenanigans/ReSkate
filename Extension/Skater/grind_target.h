#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
namespace dingosdk::grind_target {
struct Target { bool available{}, targeting{}; std::array<float,3> direction{}; };
std::optional<Target> read();
// Native target class 0..6, and the requested alignment sign. Nil releases this plugin's lease.
std::string request(std::string_view owner, std::optional<int> type, bool negative);
void release(std::string_view owner);
}
