#include "turnaround.h"
#include "turnaround_research.h"
#include "turnaround_session.h"
#include "turnaround_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_slot_override.h"
#include "Extension/Customization/item_browser.h"
#include "Extension/Customization/local_customization.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <charconv>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dingosdk::turnaround {
namespace {
Session& session() { static auto* value = new Session; return *value; } // process lifetime: its thread may outlive statics

std::span<const std::uint8_t> built_in(const wchar_t* name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&built_in), &module))
        return {};
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto* data = handle ? static_cast<const std::uint8_t*>(LockResource(handle)) : nullptr;
    return data ? std::span<const std::uint8_t>(data, SizeofResource(module, resource)) : std::span<const std::uint8_t>{};
}

std::filesystem::path game_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) { path.resize(length); break; }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}


// The board's own recipe in a skater preset (its template), and its slots, in the card's order. Read
// from the game's presets with `turnaround parts`, 2026-10-20.
constexpr std::uint32_t board_template = 0x5e61aaef;
struct BoardSlot { std::uint32_t slot; const char* label; };
constexpr BoardSlot board_slots[] = {
    {0x66071628, "Deck"}, {0x755d5ee0, "Grip tape"}, {0x976f18c4, "Grip cutout"}, {0x0add98be, "Trucks"}, {0xc83bb94e, "Wheels"}};

// An item's name as the game shows it: its catalog title, the title an item grid showed (a mod's
// item has no catalog entry), or its key made readable the way the game does for a mod's item.
std::string item_title(const std::string& asset) {
    auto title = profile::item_display_metadata(asset, Json::object()).value("title", "");
    if (title.empty()) title = item_browser::known_title(asset);
    if (title.empty()) {
        title = asset.starts_with("Own_") || asset.starts_with("own_") ? asset.substr(4) : asset;
        std::replace(title.begin(), title.end(), '_', ' ');
    }
    return title;
}

// The selected skater preset (the native getter skater_slot_override.cpp uses), or -1. Game thread.
std::int32_t selected_preset(std::uintptr_t base, std::uintptr_t manager) {
    namespace slot = addr::skater_slot_override;
    std::array<unsigned char, slot::native_selected_prefix.size()> bytes{};
    if (!memory::read_bytes(base + slot::native_selected, bytes.data(), bytes.size()) || bytes != slot::native_selected_prefix) return -1;
    return reinterpret_cast<std::int32_t (*)(std::uintptr_t)>(base + slot::native_selected)(manager);
}

// The board the skater rides now: the board recipe of the selected preset (the first when none is
// selected), its parts in the card's order, empty slots left out. Game thread.
std::vector<Part> worn_board() {
    std::vector<Part> parts;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t manager{}, begin{}, end{};
    if (!memory::read(base + addr::engine::loadout_manager, manager) || !manager || !memory::read(manager + 0x18, begin) ||
        !memory::read(manager + 0x20, end) || begin >= end || (end - begin) % 0x48 || (end - begin) / 0x48 > 10)
        return parts;
    const auto count = static_cast<std::int32_t>((end - begin) / 0x48);
    auto preset = selected_preset(base, manager);
    if (preset < 0 || preset >= count) preset = 0;
    profile::CosmeticLoadout loadout;
    std::vector<profile_runtime::CosmeticNativeRecipe> native;
    if (!profile_runtime::read_cosmetic_recipes(reinterpret_cast<const void*>(begin + static_cast<std::uintptr_t>(preset) * 0x48), loadout, native))
        return parts;
    // The Board screen shows the item highlighted in the open tab, not the one worn: that part is the
    // highlighted one. Which part the open tab is for is the one whose worn item is in its grid.
    const auto browsing = item_browser::view();
    const auto in_open_grid = [&](const std::string& asset) {
        return browsing.open && !browsing.focused_key.empty() &&
               std::find(browsing.grid_keys.begin(), browsing.grid_keys.end(), asset) != browsing.grid_keys.end();
    };
    for (const auto& recipe : loadout.recipes) {
        if (recipe.template_key != board_template) continue;
        for (const auto& wanted : board_slots)
            for (const auto& item : recipe.items) {
                if (item.slot != wanted.slot || item.asset.empty()) continue;
                if (in_open_grid(item.asset))
                    parts.push_back({wanted.label, browsing.focused_title.empty() ? item_title(browsing.focused_key) : browsing.focused_title});
                else
                    parts.push_back({wanted.label, item_title(item.asset)});
            }
        break;
    }
    logging::log(logging::Level::info, logging::Channel::customization, "Turnaround: preset {} of {}, {} board part(s).", preset, count, parts.size());
    return parts;
}

// What the skater wears now, for the Skaters screen's turnaround: every item of the selected preset
// but the board's, labelled by its catalog category (the game's names for its grids: "tops",
// "headgear"...), clothes and accessories only (not the body, hair, tattoos or gestures), in the
// order they are worn from head to foot. Game thread.
struct OutfitKind { const char* word; const char* label; int order; };
constexpr OutfitKind outfit_kinds[] = {
    {"head", "Headwear", 0}, {"hat", "Headwear", 0}, {"cap", "Headwear", 0}, {"beanie", "Headwear", 0}, {"helmet", "Headwear", 0},
    {"eye", "Eyewear", 1}, {"glass", "Eyewear", 1}, {"mask", "Face", 1},
    {"neck", "Neckwear", 2}, {"jewel", "Jewellery", 2}, {"earring", "Earrings", 2},
    {"costume", "Costume", 3}, {"outfit", "Outfit", 3},
    {"outer", "Outerwear", 4}, {"jacket", "Outerwear", 4}, {"top", "Top", 5}, {"shirt", "Top", 5}, {"hood", "Top", 5},
    {"glove", "Gloves", 6}, {"wrist", "Wrist", 6}, {"watch", "Wrist", 6}, {"bag", "Bag", 7}, {"backpack", "Bag", 7},
    {"bottom", "Bottoms", 8}, {"pant", "Bottoms", 8}, {"short", "Bottoms", 8}, {"leg", "Bottoms", 8},
    {"sock", "Socks", 9}, {"foot", "Shoes", 10}, {"shoe", "Shoes", 10}};
constexpr const char* not_outfit[] = {"skin", "face_", "body", "hair", "beard", "brow", "makeup", "eyecolo", "eye_colo",
                                      "tattoo", "gesture", "emote", "voice", "bkobject", "board", "deck", "truck", "wheel", "grip",
                                      "locomotion", "lipcolo", "mole", "stubble", "freckle", "scar", "nose", "mouth", "teeth"};
std::string folded(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::vector<Part> worn_outfit() {
    std::vector<Part> parts;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t manager{}, begin{}, end{};
    if (!memory::read(base + addr::engine::loadout_manager, manager) || !manager || !memory::read(manager + 0x18, begin) ||
        !memory::read(manager + 0x20, end) || begin >= end || (end - begin) % 0x48 || (end - begin) / 0x48 > 10)
        return parts;
    const auto count = static_cast<std::int32_t>((end - begin) / 0x48);
    auto preset = selected_preset(base, manager);
    if (preset < 0 || preset >= count) preset = 0;
    profile::CosmeticLoadout loadout;
    std::vector<profile_runtime::CosmeticNativeRecipe> native;
    if (!profile_runtime::read_cosmetic_recipes(reinterpret_cast<const void*>(begin + static_cast<std::uintptr_t>(preset) * 0x48), loadout, native))
        return parts;
    const auto& known = profile_runtime::cosmetic_runtime().items;
    std::vector<std::pair<int, Part>> found;
    std::string seen;
    for (const auto& recipe : loadout.recipes) {
        if (recipe.template_key == board_template) continue;
        for (const auto& item : recipe.items) {
            if (item.asset.empty()) continue;
            const auto entry = known.find(item.asset);
            const auto category = folded(entry != known.end() ? entry->second.category : std::string());
            seen += std::format("\n    {:#x}/{:#010x} {} [{}]", recipe.template_key, item.slot, item.asset, category);
            const auto has = [&](const char* word) { return category.find(word) != std::string::npos; };
            // The body's own parts are "cust..." (custhead, custeye, custnoses, custbodies, custmoles...).
            if (category.empty() || category.starts_with("cust") || std::any_of(std::begin(not_outfit), std::end(not_outfit), has))
                continue;
            const auto kind = std::find_if(std::begin(outfit_kinds), std::end(outfit_kinds), [&](const OutfitKind& k) { return has(k.word); });
            std::string label;
            int order = 20;
            if (kind != std::end(outfit_kinds)) label = kind->label, order = kind->order;
            else {
                // A category this does not know: its name made readable.
                label = entry->second.category;
                std::replace(label.begin(), label.end(), '_', ' ');
                if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            }
            found.push_back({order, {label, item_title(item.asset)}});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& entry : found) parts.push_back(std::move(entry.second));
    logging::log(logging::Level::info, logging::Channel::customization, "Turnaround: preset {} of {}, {} outfit item(s) of:{}", preset,
                 count, parts.size(), seen);
    return parts;
}

// Notices for the turnaround's progress, raised from the render thread's poll: once for each phase
// of each turnaround (`runs` counts them).
std::atomic<unsigned> runs{};
void report(const Progress& now) {
    static unsigned last_run = 0;
    static Phase last = Phase::idle;
    const auto run = runs.load();
    if (now.phase == last && run == last_run) return;
    last = now.phase, last_run = run;
    switch (now.phase) {
    case Phase::capturing: overlay::notify(overlay::NoticeLevel::info, "Fit Check", now.message + " Hands off for a few seconds."); break;
    case Phase::encoding: overlay::notify(overlay::NoticeLevel::info, "Fit Check", "Saving..."); break;
    case Phase::done: overlay::notify(overlay::NoticeLevel::info, "Fit check saved", now.message); break;
    case Phase::failed: overlay::notify(overlay::NoticeLevel::warning, "Fit Check", now.message); break;
    default: break;
    }
    logging::log(now.phase == Phase::failed ? logging::Level::warning : logging::Level::info, logging::Channel::graphics,
                 "Turnaround: {}", now.message);
}

bool frame_grab_active() noexcept {
    try {
        report(session().progress());
    } catch (...) {}
    return research::grab_requested() || session().busy();
}
bool frame_grab_wants() noexcept { return research::grab_requested() || session().wants_frame(); }
void frame_grab_take(const overlay::GrabbedFrame& frame) noexcept {
    if (research::grab_requested()) {
        research::take_grab(frame);
        return;
    }
    session().take_frame({frame.data, frame.width, frame.height, frame.pitch, frame.bgra, frame.depth, frame.depth_pitch});
}
bool frame_grab_banner(char* text, std::size_t size, bool* recording) noexcept {
    try {
        const auto progress = session().progress();
        std::string line;
        if (progress.phase == Phase::capturing)
            line = progress.step ? std::format("RECORDING FIT CHECK  {} frames  -  V to stop", progress.step)
                                 : std::string("RECORDING FIT CHECK  -  hands off  -  V to stop");
        else if (progress.phase == Phase::encoding) line = "SAVING FIT CHECK...";
        else return false;
        *recording = progress.phase == Phase::capturing;
        std::snprintf(text, size, "%s", line.c_str());
        return session().busy();
    } catch (...) { return false; }
}
constexpr overlay::FrameGrabHost frame_grab_host{&frame_grab_active, &frame_grab_wants, &frame_grab_take, &frame_grab_banner};
} // namespace

const overlay::FrameGrabHost* frame_grab() noexcept { return &frame_grab_host; }

namespace {
// The player's choices, kept between sessions: <Local AppData>\ReSkate\turnaround\settings.txt,
// one key=value a line, the keys set_option takes.
std::filesystem::path settings_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"turnaround" / L"settings.txt";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::mutex& options_mutex() { static auto* value = new std::mutex; return *value; }
std::string hex(Rgb colour) { return std::format("#{:02x}{:02x}{:02x}", colour[0], colour[1], colour[2]); }

// Applies one setting to `o`; what to tell the player, or empty when the key or value is not one.
std::string apply(Options& o, std::string_view key, std::string_view value) {
    const auto number = [&](int low, int high) -> std::optional<int> {
        int n{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (error != std::errc{} || end != value.data() + value.size() || n < low || n > high) return std::nullopt;
        return n;
    };
    const bool yes = value == "on" || value == "1" || value == "true", no = value == "off" || value == "0" || value == "false";
    if (key == "format") {
        if (value == "gif") o.format = Format::gif;
        else if (value == "apng" || value == "png") o.format = Format::apng;
        else if (value == "webm") o.format = Format::webm;
        else return {};
        return std::string("Format: ") + (o.format == Format::gif ? "GIF" : o.format == Format::apng ? "APNG" : "WebM") + ".";
    }
    if (key == "background") {
        if (value == "clear" || value == "transparent") o.transparent = true;
        else if (value == "matte") o.transparent = false;
        else return {};
        return o.transparent ? "Background: transparent." : "Background: matte " + hex(o.matte) + ".";
    }
    if (key == "matte") {
        auto text = value;
        if (text.starts_with('#')) text.remove_prefix(1);
        unsigned v{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
        if (text.size() != 6 || error != std::errc{} || end != text.data() + text.size()) return {};
        o.matte = {static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
        return "Matte colour: " + hex(o.matte) + ".";
    }
    if (key == "parts") {
        if (!yes && !no) return {};
        o.parts = yes;
        return o.parts ? "The parts card is on." : "The parts card is off.";
    }
    if (key == "speed") {
        const auto n = number(5, 100);
        if (!n) return {};
        o.spin = static_cast<float>(*n) / 100.0f * (o.spin < 0 ? -1.0f : 1.0f);
        return std::format("Spin speed: {}%.", *n);
    }
    if (key == "direction") {
        if (value != "left" && value != "right") return {};
        o.spin = std::abs(o.spin) * (value == "left" ? -1.0f : 1.0f);
        return std::string("Spins to the ") + std::string(value) + ".";
    }
    if (key == "axis") {
        if (value == "lx") o.spin_axis = 0;
        else if (value == "ly") o.spin_axis = 1;
        else if (value == "rx") o.spin_axis = 2;
        else if (value == "ry") o.spin_axis = 3;
        else return {};
        return "Spin stick: " + std::string(value) + ".";
    }
    if (key == "seconds") {
        const auto n = number(1, 12);
        if (!n) return {};
        o.seconds = static_cast<float>(*n);
        return std::format("One turn takes {} s.", *n);
    }
    if (key == "size") {
        const auto n = number(64, 2048);
        if (!n) return {};
        o.size = static_cast<std::uint32_t>(*n);
        return std::format("Size: {} px.", *n);
    }
    if (key == "fps") {
        const auto n = number(5, 60);
        if (!n) return {};
        o.fps = static_cast<std::uint32_t>(*n);
        return std::format("{} frames a second.", *n);
    }
    return {};
}

void save(const Options& o) {
    const auto path = settings_path();
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::trunc);
    out << "format=" << extension(o.format) << "\n"
        << "background=" << (o.transparent ? "clear" : "matte") << "\n"
        << "matte=" << hex(o.matte) << "\n"
        << "parts=" << (o.parts ? "on" : "off") << "\n"
        << "speed=" << std::lround(std::abs(o.spin) * 100) << "\n"
        << "direction=" << (o.spin < 0 ? "left" : "right") << "\n"
        << "axis=" << (o.spin_axis == 0 ? "lx" : o.spin_axis == 1 ? "ly" : o.spin_axis == 3 ? "ry" : "rx") << "\n"
        << "seconds=" << std::lround(o.seconds) << "\n"
        << "size=" << o.size << "\n"
        << "fps=" << o.fps << "\n";
}
} // namespace

Options& options() noexcept {
    static Options kept = [] {
        Options o;
        try {
            std::ifstream in(settings_path());
            for (std::string line; std::getline(in, line);) {
                const auto equals = line.find('=');
                if (equals != std::string::npos) {
                    auto key = std::string_view(line).substr(0, equals), value = std::string_view(line).substr(equals + 1);
                    if (key == "format" && value == "png") value = "apng";
                    apply(o, key, value);
                }
            }
        } catch (...) {}
        return o;
    }();
    return kept;
}

std::string set_option(std::string_view key, std::string_view value) {
    std::lock_guard lock(options_mutex());
    auto& o = options();
    auto said = apply(o, key, value);
    if (said.empty()) return "turnaround set format gif|apng|webm | background clear|matte | matte #rrggbb | parts on|off | "
                             "speed 5-100 | direction left|right | seconds 1-12 | fps 5-60 | size 64-2048";
    try { save(o); } catch (...) {}
    return said;
}

Options current_options() {
    std::lock_guard lock(options_mutex());
    return options();
}

std::string last_message() {
    const auto progress = session().progress();
    if (progress.phase == Phase::capturing) return std::format("Recording... {} frames", progress.step);
    return progress.message;
}

namespace {
std::atomic<bool (*)(const char*)>& console_queue() {
    static std::atomic<bool (*)(const char*)> value{};
    return value;
}
} // namespace
void set_console_queue(bool (*queue)(const char* command)) noexcept { console_queue().store(queue); }
void queue_console(const char* command) noexcept {
    if (const auto queue = console_queue().load()) {
        try { queue(command); } catch (...) {}
    }
}

void open_folder() {
    const auto folder = game_folder() / L"Fit Checks";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string start_export(Subject subject) {
    if (session().busy()) return "A fit check is already being made.";
    try {
        Session::Inputs in;
        in.options = current_options();
        in.options.subject = subject;
        // A GIF's frame times are in hundredths of a second, and players slow anything under two to a
        // tenth: 50 a second is the most a GIF plays at.
        if (in.options.format == Format::gif) in.options.fps = std::min<std::uint32_t>(in.options.fps, 50);
        in.options.frames = static_cast<std::uint32_t>(
            std::clamp(std::lround(in.options.seconds * static_cast<float>(in.options.fps)), 8L, static_cast<long>(most_frames)));
        if (subject == Subject::skater) {
            in.parts = worn_outfit();
            in.title = "skater";
        } else {
            in.parts = worn_board();
            const auto deck = std::find_if(in.parts.begin(), in.parts.end(), [](const Part& part) { return part.slot == "Deck"; });
            in.title = deck != in.parts.end() ? deck->name : "board";
        }
        in.fonts = {built_in(L"FONT_HEADING"), built_in(L"FONT_BODY"), built_in(L"FONT_BRUSH")};
        if (in.fonts.heading.empty() || in.fonts.body.empty() || in.fonts.brush.empty()) in.options.parts = false;
        in.folder = game_folder() / L"Fit Checks";
        in.stage = board_stage(in.options);
        ++runs;
        if (!session().start(std::move(in))) return "A fit check is already being made.";
        const auto progress = session().progress();
        if (progress.phase == Phase::failed) return progress.message;
        return std::string(subject == Subject::skater ? "Turning the skater" : "Turning the board") +
               "; the file goes to the Fit Checks folder beside Skate.exe.";
    } catch (const std::exception& error) {
        return std::string("The fit check could not start: ") + error.what();
    }
}

namespace {
std::atomic<bool> game_menu{};
}
void set_game_menu(bool up) noexcept { game_menu.store(up); }
bool v_turnaround(Subject& subject) {
    const auto browsing = item_browser::view();
    if (browsing.open) {
        if (!item_browser::board_item(browsing.focused_key, browsing.focused_category)) return false;
        subject = Subject::board;
        return true;
    }
    if (!game_menu.load()) return false;
    subject = Subject::skater;
    return true;
}

void cancel_export() noexcept { session().cancel(); }
bool exporting() noexcept { return session().busy(); }
} // namespace dingosdk::turnaround
