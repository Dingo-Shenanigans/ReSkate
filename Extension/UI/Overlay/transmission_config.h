#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::overlay::detail
{
struct TransmissionRecipe
{
    std::string shader;
    std::vector<char> mask;
    std::array<float, 8> parameters{20, 4, 2, 1.52f, 12, .0006f, 1, 1};
};

// Canonical SHA-256 of executable DXIL and its required interface chunks. Debug
// reflection can be stripped without changing identity; altered code cannot.
std::string transmission_shader_key(std::span<const char> shader);
// Adds one mod atomically. Earlier declarations retain duplicate shader keys.
// Throws before changing the destination when the launch-wide budget is
// exceeded.
std::size_t append_transmission_recipes(std::vector<TransmissionRecipe> &destination,
                                        std::vector<TransmissionRecipe> candidates);
std::vector<TransmissionRecipe> read_transmission_config(const std::filesystem::path &mod);
std::vector<TransmissionRecipe> parse_transmission_config(std::string_view text, const std::filesystem::path &mod);
} // namespace dingosdk::overlay::detail
