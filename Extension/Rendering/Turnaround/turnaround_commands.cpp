#include "Extension/Console/commands.h"
#include "turnaround.h"
#include "turnaround_research.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Settings/named_settings.h"
#include <cctype>
#include <map>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dingosdk::console {
namespace {
double number(std::istringstream& words, double fallback) {
    std::string word;
    if (!(words >> word)) return fallback;
    double value{};
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    return error == std::errc{} && end == word.data() + word.size() ? value : fallback;
}
// The engine's switches the turnaround turns off while it records, as found this session (the names
// come from the game's own settings registry):
//  - the screen-space contact shadows (the board's shadow where it nears the backdrop), not the
//    shadow maps: without those every light reaches the board unblocked and it is blown out;
//  - post effects that spread the board into the backdrop: bloom (a white deck glows onto it), lens
//    flare and dirt, film grain, chromatic aberration, motion blur;
//  - for a skater (`floor`), what puts them on the floor: screen-space reflections (the Skaters
//    screen's floor is glossy) and ambient occlusion (the dark smudge round the feet).
std::vector<std::string> shadow_switches(bool floor) {
    static constexpr const char* how[] = {"cache", "16bit", "logspace", "accum", "stencil", "transition", "viewdistance",
                                          "fixedmovement", "refine", "culling", "displacement", "tessellation", "zbuffer",
                                          "expression", "emitter", "quad", "cloud", "volumetric", "participating", "transparent"};
    static constexpr const char* effects[] = {"bloom", "lensflare", "lensdirt", "filmgrain", "chromaticaberration", "motionblur"};
    // The game's own names (from `fitcheck settings`, 2026-10-21): screen-space reflections, which
    // put the skater on the glossy floor, and the dynamic ambient occlusion round their feet.
    static constexpr const char* floors[] = {"postprocess.screenspaceraytraceenable", "postprocess.dynamicaoenable"};
    std::vector<std::string> out;
    for (const auto& name : named_setting_names()) {
        std::string folded = name;
        for (auto& c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!folded.ends_with("enable") || folded.starts_with("fog.")) continue;
        // Only the screen-space contact shadows: with the shadow maps off, every light reaches the
        // board unblocked and it comes out blown out (seen 2026-10-21).
        const bool shadow = folded.find("screenspacecontactshadows") != std::string::npos &&
                            std::none_of(std::begin(how), std::end(how), [&](const char* part) { return folded.find(part) != std::string::npos; });
        const bool effect = std::any_of(std::begin(effects), std::end(effects), [&](const char* part) { return folded.find(part) != std::string::npos; });
        const bool on_floor = floor && std::any_of(std::begin(floors), std::end(floors), [&](const char* exact) { return folded == exact; });
        if (shadow || effect || on_floor) out.push_back(name);
    }
    return out;
}
bool on(const std::string& value) {
    std::string v = value;
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v == "true" || v == "1" || v == "on";
}
// What the turnaround turned off, and what each was, to put back.
std::map<std::string, std::string>& shadows_put_out() {
    static auto* value = new std::map<std::string, std::string>;
    return *value;
}
} // namespace

void register_turnaround_commands(Commands& registry) {
    auto arguments = argument("verb_and_arguments", Type::text, true);
    arguments.rest = true;
    arguments.complete = [](const Model&, auto) { return std::vector<std::string>{"export", "set", "open", "cancel", "grab", "boards", "parts", "stick"}; };
    auto command = action("turnaround",
                          "Fit Check, spinning pictures of the board or skater (V on the Board or Skaters screen): export [skater] [gif|apng|webm] [clear|matte] [frames] [size] [parts|noparts] [key=value], "
                          "set <key> <value> (format, background, matte, parts, speed, direction, seconds, fps, size; kept), open (the folder), cancel. Research aids: grab (save this frame to Fit Checks\\), boards [seconds] "
                          "(find the board on show; spin it while this watches), parts (the items worn now), stick <x> <y> [seconds] (push the right "
                          "stick, -1..1). Answers go to the log.",
                          Group::console, {std::move(arguments)});
    command.run = [](const Model&, const Values& values, const Output& out) {
        std::istringstream words(values.empty() ? std::string{} : std::get<std::string>(values[0]));
        std::string verb;
        words >> verb;
        if (verb == "export") {
            // One-word settings for this and later turnarounds: gif, webm, matte, noparts, 72, 768...
            // "skater" turns the skater (the Skaters screen) rather than the board.
            auto subject = turnaround::Subject::board;
            for (std::string word; words >> word;) {
                if (word == "skater") { subject = turnaround::Subject::skater; continue; }
                if (word == "board") continue;
                const auto equals = word.find('=');
                if (equals != std::string::npos) { turnaround::set_option(word.substr(0, equals), word.substr(equals + 1)); continue; }
                if (word == "gif" || word == "apng" || word == "png" || word == "webm") turnaround::set_option("format", word);
                else if (word == "clear" || word == "transparent" || word == "matte") turnaround::set_option("background", word);
                else if (word == "parts" || word == "noparts") turnaround::set_option("parts", word == "parts" ? "on" : "off");
                else if (std::uint32_t n{}; std::from_chars(word.data(), word.data() + word.size(), n).ec == std::errc{} && n)
                    turnaround::set_option(n <= 12 ? "seconds" : "size", word);
            }
            out(turnaround::start_export(subject));
        } else if (verb == "set") {
            std::string key, value;
            words >> key >> value;
            out(turnaround::set_option(key, value));
        } else if (verb == "shadows") {
            // `turnaround shadows off|restore`: queued by the turnaround around its capture, so the board's
            // shadow on the backdrop and its bloom do not come out as part of it. On the game thread, as
            // settings are.
            std::string what, subject;
            words >> what >> subject;
            auto& put_out = shadows_put_out();
            if (what == "off") {
                for (const auto& name : shadow_switches(subject == "skater")) {
                    const auto value = named_setting_value(name);
                    if (!value || !on(*value) || put_out.contains(name)) continue;
                    const auto said = request_named_setting(name, "false", false);
                    if (said.starts_with("error")) continue;
                    put_out[name] = *value;
                    logging::log(logging::Level::info, logging::Channel::graphics, "Turnaround: {} off while turning ({})", name, said);
                }
                out(put_out.empty() ? "Nothing to turn off." : std::to_string(put_out.size()) + " shadow and effect switch(es) off.");
            } else {
                for (const auto& [name, value] : put_out) request_named_setting(name, value, false);
                out(put_out.empty() ? "Nothing to put back." : "Shadows and effects put back.");
                put_out.clear();
            }
        } else if (verb == "open") {
            turnaround::open_folder();
            out("Opened the Fit Checks folder.");
        } else if (verb == "cancel") {
            turnaround::cancel_export();
            out(turnaround::exporting() ? "Cancelling the fit check." : "No fit check is being made.");
        } else if (verb == "depth") {
            // Research aid: the game's depth buffers over the next two frames, as pictures in the Fit
            // Checks folder with depth-probe.txt (Extension/UI/Overlay/depth_probe.cpp).
            overlay::request_depth_probe(turnaround::research::folder().c_str());
            out("Taking the game's depth buffers over the next frames: depth-probe.txt and depth-*.pgm go to the Fit Checks folder.");
        } else if (verb == "settings") {
            // Research aid: every engine setting the game has, with its value, to a file beside the
            // fit checks (for finding what hides the scene behind the board or skater).
            const auto folder = turnaround::research::folder();
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            std::ofstream file(folder / L"engine-settings.txt", std::ios::trunc);
            std::size_t count = 0;
            for (const auto& name : named_setting_names()) {
                const auto value = named_setting_value(name);
                file << name << " = " << (value ? *value : std::string("?")) << "\n";
                ++count;
            }
            out(file ? std::to_string(count) + " settings written to engine-settings.txt in the Fit Checks folder."
                     : std::string("The settings file could not be written."));
        } else if (verb == "grab") {
            turnaround::research::request_grab();
            out("The next frame will be saved in the Fit Checks folder beside Skate.exe.");
        } else if (verb == "boards") {
            out(turnaround::research::request_boards(number(words, 6)));
        } else if (verb == "parts") {
            out(turnaround::research::request_parts());
        } else if (verb == "stick") {
            const auto x = number(words, 0.5), y = number(words, 0), seconds = number(words, 2);
            out(turnaround::research::request_stick(x, y, seconds));
        } else {
            out("turnaround export [...] | set <key> <value> | open | cancel | grab | settings | depth | boards [seconds] | parts | stick <x> <y> [seconds]");
        }
    };
    // The mod is called Fit Check: `fitcheck` does the same (`turnaround` stays, as the mod queues it).
    command.aliases.push_back("fitcheck");
    registry.add(std::move(command));
}
} // namespace dingosdk::console
