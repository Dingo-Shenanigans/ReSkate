#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dingosdk::replay_export {
// The quality the replay editor's MP4 export is encoded at (`replay quality`, saved with the profile as
// ReSkate.ReplayExportQuality). The engine encodes MP4 with Media Foundation's H.264 encoder at a bitrate
// the game picks; quality_game leaves it so. The others set the bitrate from the export's size and frame
// rate, as bits for each pixel of each frame: at 1080p60 about 6, 12, 25 and 45 Mbit/s.
enum Quality { quality_game, quality_low, quality_medium, quality_high, quality_very_high, quality_count };
inline constexpr std::array<std::string_view, quality_count> quality_names{"game", "low", "medium", "high", "veryhigh"};
inline constexpr std::array<double, quality_count> quality_bits_per_pixel{0.0, 0.05, 0.1, 0.2, 0.36};
// For an encoder in its quality mode, which takes no bitrate: 0..100.
inline constexpr std::array<int, quality_count> quality_percent{0, 50, 70, 85, 95};
// Hardware H.264 encoders take up to about this much; below the floor a small export turns to mush.
inline constexpr int min_export_kilobits = 2000, max_export_kilobits = 100000;
// The bitrate, in kilobits a second, an export of this size and frame rate is encoded at; 0 for
// quality_game, or for a size or frame rate that makes no sense (the game's own then stands).
inline int export_kilobits(int quality, int width, int height, double fps) {
    if (quality <= quality_game || quality >= quality_count || width <= 0 || height <= 0 || !(fps > 0.0 && fps <= 1000.0)) return 0;
    const double kilobits = quality_bits_per_pixel[static_cast<std::size_t>(quality)] * width * height * fps / 1000.0;
    return static_cast<int>(std::clamp(kilobits, static_cast<double>(min_export_kilobits), static_cast<double>(max_export_kilobits)));
}


// The replay editor steps the replay by one frame of video for every frame the
// game renders, but the engine encodes at most one frame per world update and
// drops the rest. Rendering faster than it updates (an uncapped frame rate),
// an export came out with a fraction of its frames and played that much too
// fast. Every rendered frame of a replay export is encoded instead.
// MP4 export on a PC with GPUs from two vendors (an AMD processor's integrated
// graphics beside a graphics card) no longer picks the hardware encoder of the
// GPU the game does not render on.
// An MP4 export is encoded at the chosen Quality (above).
bool start(std::uintptr_t base) noexcept;
}
