#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::bmx::standalone_shadow {
enum class Issue { disabled, reset, ready, invalid_lease, invalid_clock, duplicate,
    discontinuity, geometry_mismatch, invalid_output, output_limit };
// Last shadow evaluation only; accepted_total counts shadow history commits.
// Nothing in this report authorizes native writes or proves native application.
// Counters saturate at65535; vectors/norms are diagnostic and bounded to1e9.
struct Report {
    bool enabled{},evaluated{},valid{},within_caps{},active{},grounded{},diagnostic_clamped{};
    bool comparison_available{},selection_matches{},active_matches{};
    Issue issue{Issue::disabled};
    std::uint64_t serial{},token{};
    unsigned evaluated_total{},accepted_total{},duplicate_total{},rejected_total{},reset_total{};
    unsigned shadow_selection{},current_selection{};
    std::array<double,3> force{},torque{},hop_impulse{};
    double combined_force_norm{},torque_norm{},hop_norm{},steering_angle{};
    double force_difference_norm{},torque_difference_norm{},hop_difference_norm{};
};
}
