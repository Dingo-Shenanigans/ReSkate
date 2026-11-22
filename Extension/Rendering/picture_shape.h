#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The game's picture in a shape a plugin asks for (reskate.screen.shape): a real 4:3 (or any shape), not the screen's
// picture stretched or cut. The game renders a picture of that shape when its swap chain is (RenderDevice.FullscreenWidth
// and Height, which it takes while it runs), and shows it 1:1 when its window is that size too. So the window is sized to
// it, as big as fits, in the middle of its monitor, with a black window of ReSkate's own behind it over the rest of the
// monitor (the taskbar told the game is full screen, so it stays out of the way), and the render size is the window's in
// the monitor's own pixels (with Windows' display scaling the game's threads see a scaled screen in scaled units: a 4:3
// picture at 200% was rendered at half size and stretched). All of it is put back as it was once no plugin asks for a
// shape. It needs the game in borderless full screen (the game's default). Nothing changes while a video is exported
// (Extension/Rendering/replay_export.h): its frames' size must not change under it.
namespace dingosdk::picture_shape {
// Client thread: `owner`'s shape, width over height (0.25 to 4), or none (its own given back). One owner at a time: another's
// is refused while one holds. Empty, or why not.
std::string set(std::string_view owner, std::optional<float> aspect) noexcept;
void clear(std::string_view owner) noexcept;
// Client thread (the game's update thread, which the game's settings need), every tick: the window and the render size
// kept as asked, or put back.
void tick() noexcept;

// The graphics card's memory as Windows gives this process it now (its budget and what it uses), and how much the PC can
// still commit (RAM and page file): bytes. Nothing when the card's is not known (Engine/Game/UI/game_window.h).
struct Memory {
    std::uint64_t budget{}, usage{}, commit_free{};
};
std::optional<Memory> memory() noexcept;

// Pure (tested). The size (even numbers) of a picture of `aspect` as big as fits in `width` x `height`.
struct Size {
    int width{}, height{};
    bool operator==(const Size&) const = default;
};
Size fitted(float aspect, Size monitor) noexcept;
// A window's size, as the game's threads see the screen (`seen`, in scaled units), in the monitor's own pixels: each side
// by its own share, to even numbers, within the monitor. The same when the screen is not scaled.
Size in_pixels(Size window, Size seen, Size pixels) noexcept;
} // namespace dingosdk::picture_shape
