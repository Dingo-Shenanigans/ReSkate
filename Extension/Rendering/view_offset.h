#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The camera the picture is drawn from, moved and turned by a plugin (reskate.camera.offset). The game's building of its
// views each frame (Engine/Game/Build/<build>/game_views.h) is handed the camera with the offset, so the picture, its
// shadows, culling and fog, and the renderer's record of the frame before (motion blur, temporal anti-aliasing) all come
// from the moved camera, as from a camera that really moved; the game's cameras themselves (gameplay's, the replay
// editor's, its keyframes) never see it. It holds in play, in the replay editor (on the camera the replay plays, after the
// replay's camera has had its say) and in the videos the editor exports, whose frames are built the same way: a camera
// shake, a handheld look, a field of view of the plugin's own.
//
// Only the first local player's view. A transform that is no camera's (not orthonormal) is left as it is.
namespace dingosdk::view_offset {
struct Offset {
    // Degrees: yaw (positive turns the view right), pitch (positive looks up), roll (positive tilts it clockwise, its
    // right side down). Applied roll, then pitch, then yaw, each about the camera's own axes.
    std::array<float, 3> turn{};
    // Metres along the camera's own right, up and forward.
    std::array<float, 3> move{};
    // The vertical field of view, degrees (0: the camera's own), then multiplied by `zoom` (1: as it is).
    float fov{};
    float zoom{1.0f};
};
// Client thread. The offset `owner` asks for, for the next 300 ms (so every tick); none ends its own. One owner at a time:
// another's is refused while one holds. Hooks the game's view building the first time (checked by its bytes; nothing is
// patched until an offset holds). Empty, or why not.
std::string set(std::string_view owner, std::optional<Offset> offset) noexcept;
// The owner's going (its plugin stopped): its offset ends now.
void clear(std::string_view owner) noexcept;
// Client thread, every tick: the hooks on while an offset holds, off once none has for a second (switching them suspends
// the game's threads for a moment, so not every time one lapses).
void tick() noexcept;
// Whether this build's view building is the one known (after the first set).
bool available() noexcept;

// The transform an offset moves a camera's world transform (rows right, up, back, position) to; the same when it is no
// camera's. Pure (tested).
std::array<float, 16> moved(const std::array<float, 16>& camera, const Offset& offset) noexcept;
// The vertical field of view (radians) an offset gives a camera of `fov` radians, within 0.02 to 3.0. Pure (tested).
float field_of_view(float fov, const Offset& offset) noexcept;
} // namespace dingosdk::view_offset
