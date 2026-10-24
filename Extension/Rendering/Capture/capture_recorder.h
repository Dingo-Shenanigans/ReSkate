#pragma once
#include "capture_encode.h"
#include "capture_panel.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// A recording of part of the screen as the game draws it, for reskate.capture: every presented frame
// of an area, read back before ReSkate's own UI goes on (the overlay's frame grab), cut out by the
// game's depth where it has one, with a small grey thumbnail of each frame to compare them by; then
// chosen frames saved as a GIF, APNG, PNG or WebM, on a thread of its own. Ported from Fit Check
// (pull request 326's turnaround_session.cpp), whose turn finding is now the plugin's. No Windows or
// game code: the overlay hands frames to take_frame (render thread), the plugin side asks the rest
// (any thread).
namespace dingosdk::capture {
// Where on screen: fractions of the whole screen, or of the game's menu frame (`menu`: the whole
// width up to 21:9, a 21:9 frame in the middle of anything wider), left to right and top to bottom.
struct Area {
    float left{}, top{}, right{1}, bottom{1};
    bool menu{};
};
Rect area_on(const Area& area, std::uint32_t width, std::uint32_t height) noexcept;

enum class Format { gif, apng, png, webm };
const char* extension(Format format) noexcept;

struct RecordRequest {
    Area area;
    bool depth{true};          // cut each frame out by the game's depth where it has one
    std::uint32_t height{};    // each kept picture this tall at most (0: as on screen)
    std::uint32_t skip{};      // presented frames let go first
    std::uint32_t most{2400};  // frames taken at most; then it stops by itself
    double seconds{60};        // and after this long
};
// Pictures and bytes held at once, at most: past them every other kept frame is let go, and from
// then on only every other frame taken is kept (and so on).
inline constexpr std::size_t most_kept = 480;
inline constexpr std::size_t kept_bytes = std::size_t{1000} << 20;
inline constexpr std::uint32_t most_saved = 360; // frames in one saved file
inline constexpr std::uint32_t thumbnail_width = 24, thumbnail_height = 48;

enum class State { idle, recording, stopped, saving, saved, failed };
struct Status {
    State state{State::idle};
    std::uint32_t taken{}, kept{};
    bool depth{};            // every kept frame was cut out by the game's depth
    std::string message;     // what went wrong, or what was saved
    std::filesystem::path file;
};

enum class Cutout { automatic, depth, colour, none };
struct SaveRequest {
    std::vector<std::uint32_t> frames; // frames taken (from 0), each saved as the nearest one kept
    Format format{Format::gif};
    std::uint32_t fps{30};
    std::uint32_t size{512};           // the square what was recorded is fitted into, in pixels
    std::optional<Rgb> background;     // flattened over it; none: transparent
    Cutout cutout{Cutout::automatic};  // automatic: by depth when every frame had it, else by colour
    bool solid{true};                  // a hole what turns closes all round is part of it (a board), or only a small one (a skater)
    std::string card_title;            // a card beside it: this title, then each line (none without lines)
    std::vector<Part> card;
    PanelFonts fonts;                  // for the card; must outlive the save
    std::filesystem::path file;        // where it is written (its folder is made)
};

class Recorder {
public:
    // A new recording, letting go of the last one; false (and why) while one is being saved.
    bool start(const RecordRequest& request, std::string& why);
    // No more frames; what was taken is kept for compare, kept and save.
    void stop() noexcept;
    // Lets go of everything taken.
    void discard() noexcept;
    Status status() const;
    // How unlike two frames taken look: the mean difference of their thumbnails' grey levels
    // (0..255), or a negative number when either is not known.
    float compare(std::uint32_t a, std::uint32_t b) const;
    // The frames still held, by the number they were taken as, in order.
    std::vector<std::uint32_t> kept() const;
    // Saves on a thread of its own (the recording stops first); false (and why) when there is
    // nothing to save or a save is going on.
    bool save(SaveRequest request, std::string& why);

    // Render thread, every presented frame.
    bool active() const noexcept;
    bool wants_frame() noexcept;
    void take_frame(const PixelView& frame) noexcept;

private:
    void fail(std::string message);
    void encode(SaveRequest request) noexcept;

    mutable std::mutex mutex_;
    Status status_;
    RecordRequest request_;
    Rect area_;
    std::uint32_t skip_{}, stride_{1};
    std::chrono::steady_clock::time_point started_{};
    std::vector<Image> pictures_;
    std::vector<std::uint32_t> kept_at_;
    std::vector<bool> kept_depth_;
    std::vector<std::vector<float>> thumbnails_; // every frame taken
    std::atomic<bool> saving_{false};
};

// The pieces of saving, for the tests: what turns cut out by depth into `picture`'s alpha (false when
// the depth shows nothing), a frame's grey thumbnail, and only what moved across the frames kept.
bool depth_mask(const PixelView& frame, Rect area, Image& picture);
std::vector<float> thumbnail(const PixelView& frame, Rect area);
float unlike(const std::vector<float>& a, const std::vector<float>& b) noexcept;
std::vector<std::uint8_t> unchanging(const std::vector<Image>& frames);
Rect keep_moving(std::vector<Image>& frames, const std::vector<std::uint8_t>& fixed);
} // namespace dingosdk::capture
