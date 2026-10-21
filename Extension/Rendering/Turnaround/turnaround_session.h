#pragma once
#include "turnaround_encode.h"
#include "turnaround_matte.h"
#include "turnaround_panel.h"
#include "turnaround_subject.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// One board turnaround, from the press of the button to the saved file: turns the board on show a
// step at a time through a whole turn, grabs each step's frame (twice, over a dark and a light
// background, when it is to be see-through), cuts the board out, and saves the animation.
// Driven by the presented frames (on_present / wants_frame / take_frame, render thread); the
// encoding runs on a thread of its own. The game side is a Stage, so this file knows nothing of
// the game.
namespace dingosdk::turnaround {
enum class Format { gif, apng, webm };
const char* extension(Format format) noexcept;

// The most frames one turnaround has: 6 s at 60 a second (each is held until the file is written).
inline constexpr std::uint32_t most_frames = 360;

struct Options {
    Subject subject{Subject::board}; // chosen for each turnaround, not kept
    Format format{Format::gif};
    bool transparent{true};    // no background; otherwise the board over `matte`
    Rgb matte{24, 25, 28};
    std::uint32_t frames{120}; // pictures in one whole turn (seconds x fps, set when one starts; most_frames at most)
    std::uint32_t fps{30};
    float seconds{4};          // how long one whole turn takes to play
    std::uint32_t size{512};   // the board's square, in pixels; the parts card adds to the width
    bool parts{true};          // the "parts used" card beside the board
    std::uint32_t settle{6};   // frames to wait after each change, for the game's temporal smoothing
    float spin{0.5f};          // how hard the stick is pushed to turn the board (-1..1)
    std::uint8_t spin_axis{2}; // which stick axis: 0 left x, 1 left y, 2 right x, 3 right y
};

// What the game does for a turnaround. Called on the render thread; an implementation that must act
// on the game's own thread queues the work and is given time (Options::settle frames) to apply it.
class Stage {
public:
    virtual ~Stage() = default;
    // Takes the board on show over: true, or false with the reason.
    virtual bool begin(std::string& why) = 0;
    // Turns it to `turn` (0..1 of a whole turn from where it started), over the dark or the light
    // background. A stage that cannot change the background ignores `light`.
    virtual void pose(double turn, bool light) = 0;
    virtual bool changes_background() const = 0;
    // Gives it back as it was.
    virtual void end() = 0;
    // A stage that turns the board on its own once begun (as the player's stick would), at a steady
    // speed it does not know, rather than to a pose: every frame is taken, and the session finds
    // where the turn comes round.
    virtual bool continuous() const { return false; }
    // A continuous stage that turns too fast for the frames wanted (a whole turn in fewer frames
    // than the animation has) is asked to turn slower: `ratio` of its speed now, about. False when
    // it cannot. speed_up goes back part of the way (it slowed so much it stopped).
    virtual bool slow_down(double /*ratio*/) { return false; }
    virtual bool speed_up() { return false; }
};

enum class Phase { idle, capturing, encoding, done, failed };
struct Progress {
    Phase phase{Phase::idle};
    std::uint32_t step{}, steps{};
    std::string message; // for the notice
    std::filesystem::path file;
};

class Session {
public:
    struct Inputs {
        Options options;
        std::string title;             // the deck's name, for the file name
        std::vector<Part> parts;
        PanelFonts fonts;              // must outlive the session
        std::filesystem::path folder;  // where the file goes
        std::shared_ptr<Stage> stage;
    };
    // Starts one; false (and Progress says why) while another is still running.
    bool start(Inputs inputs);
    void cancel() noexcept;
    Progress progress() const;
    bool busy() const noexcept;
    // Render thread, every presented frame: whether this frame's pixels are wanted.
    bool wants_frame() noexcept;
    void take_frame(const PixelView& frame) noexcept;

private:
    void finish(std::string message, bool failed) noexcept;
    void encode() noexcept;

    mutable std::mutex mutex_;
    Progress progress_;
    std::atomic<bool> busy_{false}, cancelled_{false};
    // Render thread state.
    Inputs in_;
    std::uint32_t angle_{}, wait_{};
    bool light_{};
    bool posed_{};
    Rect area_;              // where on screen the board can be
    std::uint32_t work_scale_{1};
    std::vector<std::uint8_t> dark_; // the dark capture of this angle, waiting for its light one
    std::uint32_t dark_pitch_{};
    std::vector<Image> captured_;
    // Continuous stages (see Stage::continuous): every frame of one turn.
    std::vector<double> kept_at_;          // each kept picture's frame
    std::vector<bool> kept_depth_;         // whether it was cut out by the game's depth
    std::vector<float> difference_;        // how unlike the start each frame looks
    std::vector<float> long_difference_;   // the same over a longer run of views
    std::vector<std::vector<float>> thumbnails_; // every frame, for coming round
    std::uint32_t taken_{}, stride_{1}, warmup_{}, slowdowns_{};
    double period_{};                      // frames in one whole turn
    std::chrono::steady_clock::time_point started_{};
    void take_continuous(const PixelView& frame);
    void turn_again(const char* message);
    bool next_continuous();
    double come_round(const std::vector<float>& small);
    // Grey levels of change in the thumbnails that say it turns: a skater fills less of their area
    // than a board does, and a slow turn changes little in the first frames.
    float moved_enough() const noexcept { return in_.options.subject == Subject::skater ? 2.5f : 6.0f; }
};
} // namespace dingosdk::turnaround
