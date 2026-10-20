#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::anim_studio {
// Engine-tick observations, not inferred animation-graph states.
struct ResearchSample {
    double seconds{};
    std::optional<std::uint32_t> physics;
    std::string trick;
    std::optional<float> board_up_speed;
    bool studio_open{}, custom_enabled{}, goofy{}, recording_armed{};
    std::optional<bool> switch_stance;
    std::uint64_t animation_updates{}, skater_publications{}, board_publications{};
    double skater_age{-1}, board_age{-1};
};
class ResearchBuffer {
public:
    static constexpr std::size_t capacity = 18000;
    static constexpr double duration = 60.0;
    std::vector<ResearchSample> samples;
    // Keep state/name/mode changes between 20 Hz periodic samples. No file I/O.
    bool append(ResearchSample sample) {
        if (samples.size() >= capacity || sample.seconds > duration) return false;
        if (!samples.empty()) {
            const auto& last = samples.back();
            if (last.physics == sample.physics && last.trick == sample.trick &&
                last.studio_open == sample.studio_open && last.custom_enabled == sample.custom_enabled &&
                last.goofy == sample.goofy && last.recording_armed == sample.recording_armed &&
                last.switch_stance == sample.switch_stance &&
                sample.seconds - last.seconds < 0.05) return true;
        }
        samples.push_back(std::move(sample));
        return samples.size() < capacity && samples.back().seconds < duration;
    }
};
} // namespace dingosdk::anim_studio
