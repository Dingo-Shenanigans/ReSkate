#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace dingosdk::anim_studio {
inline std::string_view graded_pop_name(int grade) noexcept {
    return grade == 1 ? "low" : grade == 2 ? "mid" : grade == 3 ? "high" : "";
}
inline std::uint64_t pop_grade_since(std::uint64_t now, double elapsed_seconds) noexcept {
    const double age = std::isfinite(elapsed_seconds) ? std::max(0.0, elapsed_seconds) * 1000.0 + 300.0 : static_cast<double>(now);
    return age >= static_cast<double>(now) ? 0 : now - static_cast<std::uint64_t>(age);
}
enum class TakeRejection { none, missing_trick, wrong_trick, missing_board, invalid_pop, missing_grade, wrong_pop };
struct TakeDecision { std::string pop; TakeRejection rejection{}; };
inline TakeDecision evaluate_recorded_take(bool scripted, std::string_view observed, std::string_view expected,
                                          std::string_view requested_pop, int grade, bool board, float air) {
    TakeDecision result;
    const auto graded = graded_pop_name(grade);
    if (scripted) result.pop = requested_pop;
    else if (!graded.empty()) result.pop = graded;
    else if (std::isfinite(air) && air > 0) result.pop = air < 0.45f ? "low" : air < 0.65f ? "mid" : "high";
    if (observed.empty()) result.rejection = TakeRejection::missing_trick;
    else if (scripted && observed != expected) result.rejection = TakeRejection::wrong_trick;
    else if (!board) result.rejection = TakeRejection::missing_board;
    else if (result.pop != "low" && result.pop != "mid" && result.pop != "high") result.rejection = TakeRejection::invalid_pop;
    else if (scripted && graded.empty()) result.rejection = TakeRejection::missing_grade;
    else if (scripted && graded != requested_pop) result.rejection = TakeRejection::wrong_pop;
    return result;
}
} // namespace dingosdk::anim_studio
