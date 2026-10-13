#include "game_settings.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dingosdk::launcher_game_settings {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

constexpr std::size_t index_entry = 0x50, index_name = 32;
constexpr std::size_t max_container = 16 * 1024 * 1024;

[[noreturn]] void fail(const char* what) { throw std::runtime_error(what); }

std::uint64_t u64_at(std::span<const unsigned char> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 8) fail("The game's settings file is cut short");
    std::uint64_t value{};
    std::memcpy(&value, bytes.data() + at, 8);
    return value;
}
void put_u64(Bytes& bytes, std::size_t at, std::uint64_t value) { std::memcpy(bytes.data() + at, &value, 8); }

// A size as the object format writes it: seven bits a byte, the low ones first.
std::uint64_t varint(std::span<const unsigned char> bytes, std::size_t& at) {
    std::uint64_t value{};
    for (int shift = 0; shift < 63; shift += 7) {
        if (at >= bytes.size()) fail("The game's settings file is cut short");
        const auto byte = bytes[at++];
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return value;
    }
    fail("The game's settings file has a size that is too long");
}
void put_varint(Bytes& out, std::uint64_t value) {
    do {
        const auto byte = static_cast<unsigned char>(value & 0x7f);
        value >>= 7;
        out.push_back(value ? static_cast<unsigned char>(byte | 0x80) : byte);
    } while (value);
}

// The body section of a container: where it starts in DATA, and where INDEX describes it.
struct Body {
    std::size_t entry{};    // offset of its INDEX entry
    std::size_t offset{};   // of the section in DATA
    std::size_t size{};     // the section: a 16-byte head, then the object
};
Body find_body(const Container& container) {
    const auto& index = container.index;
    if (index.empty() || index.size() % index_entry) fail("The game's settings index is not in the expected format");
    Body body;
    bool found{};
    std::uint64_t last_end{};
    for (std::size_t at = 0; at < index.size(); at += index_entry) {
        const auto size = u64_at(index, at + index_name + 8), offset = u64_at(index, at + index_name + 16),
                   end = u64_at(index, at + index_name + 24);
        if (offset > container.data.size() || size > container.data.size() - offset || end != offset + size)
            fail("The game's settings index does not match its data");
        last_end = std::max(last_end, end);
        if (std::strncmp(reinterpret_cast<const char*>(index.data() + at), "body", index_name) != 0) continue;
        body = {at, static_cast<std::size_t>(offset), static_cast<std::size_t>(size)};
        found = true;
    }
    // The body is rewritten at another length, so nothing may follow it.
    if (!found || body.size < 18 || body.offset + body.size != last_end)
        fail("The game's settings file is not laid out as expected");
    if (u64_at(container.data, body.offset) != body.size - 16) fail("The game's settings body has the wrong length");
    return body;
}

// One entry of the body's object: its type byte, name and the bytes of its value.
struct Entry {
    unsigned type{};
    std::string_view name;
    std::size_t start{}, value{}, end{};
};
// Every entry, in order, and where the object's entries begin. Throws on anything unknown:
// a container that cannot be walked whole is not rewritten.
struct Walked {
    std::vector<Entry> entries;
    std::size_t begin{};
};
Walked walk(const Container& container, const Body& body) {
    const std::span<const unsigned char> data(container.data.data(), body.offset + body.size);
    std::size_t at = body.offset + 16;
    // An unnamed object: its type, the size of what follows, entries, a zero.
    if ((data[at++] & 0x1f) != 2) fail("The game's settings body is not an object");
    const auto object_size = varint(data, at);
    if (object_size == 0 || object_size != data.size() - at) fail("The game's settings object has the wrong length");
    Walked walked;
    walked.begin = at;
    const auto skip = [&](std::uint64_t count) {
        if (count > data.size() - at) fail("The game's settings file is cut short");
        at += static_cast<std::size_t>(count);
    };
    while (at < data.size() && data[at] != 0) {
        Entry entry;
        entry.start = at;
        entry.type = data[at++] & 0x1f;
        const auto name_end = std::find(data.begin() + static_cast<std::ptrdiff_t>(at), data.end(), static_cast<unsigned char>(0));
        if (name_end == data.end()) fail("The game's settings file is cut short");
        entry.name = std::string_view(reinterpret_cast<const char*>(data.data() + at), static_cast<std::size_t>(name_end - data.begin()) - at);
        at += entry.name.size() + 1;
        entry.value = at;
        switch (entry.type) {
        case 1: case 2: case 0x13: skip(varint(data, at)); break;   // list, object, blob
        case 6: skip(1); break;                                     // bool
        case 7: {                                                   // text: length with its zero, text, zero
            const auto length = varint(data, at);
            if (length == 0) fail("The game's settings file has text without its end");
            skip(length);
            break;
        }
        case 8: case 0xb: skip(4); break;                           // int32, float
        case 9: case 0xc: skip(8); break;                           // int64, double
        case 0xf: skip(16); break;                                  // guid
        case 0x10: skip(20); break;                                 // sha1
        default: fail("The game's settings hold a kind of value this launcher does not know");
        }
        entry.end = at;
        walked.entries.push_back(entry);
    }
    if (at + 1 != data.size()) fail("The game's settings object does not end where it should");
    return walked;
}

Bytes read_file(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > max_container) fail("Cannot read the game's settings file");
    Bytes bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) && !bytes.empty())
        fail("Cannot read the game's settings file");
    return bytes;
}

// Written beside the file and moved over it, so a failed write leaves the old file.
void write_file(const fs::path& path, const Bytes& bytes) {
    auto temporary = path;
    temporary += L".reskate-new";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            output.close();
            DeleteFileW(temporary.c_str());
            fail("Cannot write the game's settings file");
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        fail("Cannot replace the game's settings file; close skate. first");
    }
}

constexpr Choice quality[]{{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"ultra", "Ultra"}};
#define DINGO_QUALITY(prefix) \
    {{prefix "_low", "Low"}, {prefix "_medium", "Medium"}, {prefix "_high", "High"}, {prefix "_ultra", "Ultra"}}
constexpr Choice frame_rate[]{{"target_30_present_tltr", "30"}, {"target_60_present_tltr", "60"},
    {"target_120_present", "120"}, {"target_uncapped_present_tltr", "Unlimited"}};
constexpr Choice vsync[]{{"v_sync_off", "Off"}, {"v_sync_on", "On"}, {"v_sync_1_2", "Every 2nd refresh"},
    {"v_sync_1_3", "Every 3rd refresh"}, {"v_sync_1_4", "Every 4th refresh"}};
constexpr Choice scaling[]{{"resolution_scaling_off", "Off"}, {"resolution_scaling_on", "On"}};
constexpr Choice scale[]{{"resolution_scale_0_6_to_1", "60%"}, {"resolution_scale_0_7_to_1", "70%"},
    {"resolution_scale_0_8_to_1", "80%"}, {"resolution_scale_0_9_to_1", "90%"}, {"resolution_scale_1", "100%"}};
constexpr Choice upscaler[]{{"frame_synthesis_mode_none", "Off"}, {"frame_synthesis_mode_dlss", "NVIDIA DLSS"},
    {"frame_synthesis_mode_fsr", "AMD FSR"}, {"frame_synthesis_mode_xe_ss", "Intel XeSS"}};
constexpr Choice dlss[]{{"frame_synthesis_quality_dlss_ultra_performance", "Ultra Performance"},
    {"frame_synthesis_quality_dlss_performance", "Performance"}, {"frame_synthesis_quality_dlss_balanced", "Balanced"},
    {"frame_synthesis_quality_dlss_quality", "Quality"}, {"frame_synthesis_quality_dlss_dlaa", "DLAA (native)"}};
constexpr Choice fsr[]{{"frame_synthesis_quality_fsr3_performance", "Performance"},
    {"frame_synthesis_quality_fsr3_balanced", "Balanced"}, {"frame_synthesis_quality_fsr3_quality", "Quality"},
    {"frame_synthesis_quality_fsr3_ultra_quality", "Ultra Quality"}, {"frame_synthesis_quality_fsr3_native", "Native"}};
constexpr Choice xess[]{{"frame_synthesis_quality_xe_ss_ultra_performance", "Ultra Performance"},
    {"frame_synthesis_quality_xe_ss_performance", "Performance"}, {"frame_synthesis_quality_xe_ss_balanced", "Balanced"},
    {"frame_synthesis_quality_xe_ss_quality", "Quality"}, {"frame_synthesis_quality_xe_ss_ultra_quality", "Ultra Quality"},
    {"frame_synthesis_quality_xe_ss_ultra_quality_plus", "Ultra Quality Plus"},
    {"frame_synthesis_quality_xe_ss_native", "Native"}};
constexpr Choice antialiasing[]{{"antialiasing_off", "Off"}, {"antialiasing_fxaa_medium", "FXAA"},
    {"antialiasing_temporal", "TAA"}};
constexpr Choice texture_filtering[] DINGO_QUALITY("texture_filtering");
constexpr Choice texture_quality[] DINGO_QUALITY("texture_quality");
constexpr Choice gibs[]{{"gibs_off", "Off"}, {"gibs_on", "On"}};
constexpr Choice gi_grid[] DINGO_QUALITY("gi_grid");
constexpr Choice lighting[] DINGO_QUALITY("lighting");
constexpr Choice shadows[] DINGO_QUALITY("shadows");
constexpr Choice mesh[] DINGO_QUALITY("mesh_quality");
constexpr Choice effects[] DINGO_QUALITY("effects_quality");
constexpr Choice post_process[] DINGO_QUALITY("post_process");
constexpr Choice occlusion[]{{"ambient_occlusion_off", "Off"}, {"ambient_occlusion_ssao", "SSAO"},
    {"ambient_occlusion_gtao_half", "GTAO, half resolution"}, {"ambient_occlusion_gtao_full", "GTAO, full resolution"}};
constexpr Choice depth_of_field[]{{"depth_of_field_off", "Off"}, {"depth_of_field_low", "Low"},
    {"depth_of_field_medium", "Medium"}, {"depth_of_field_high", "High"}};
constexpr Choice motion_blur[]{{"motion_blur_off", "Off"}, {"motion_blur_low", "Low"}, {"motion_blur_medium", "Medium"},
    {"motion_blur_high", "High"}, {"motion_blur_ultra", "Ultra"}};
#undef DINGO_QUALITY

// The game's own list (its tier manager's user settings), in its menu's order.
constexpr Setting all_options[]{
    {"FRAME RATE", "Target Framerate", "Frame rate limit", frame_rate, "", ""},
    {"FRAME RATE", "VSync", "VSync", vsync, "", ""},
    {"RESOLUTION SCALING", "Resolution Scale Enable", "Dynamic resolution", scaling, "", ""},
    {"RESOLUTION SCALING", "Resolution", "Lowest resolution", scale, "Resolution Scale Enable", "resolution_scaling_on"},
    {"UPSCALING", "Frame Synthesis Mode", "Upscaler", upscaler, "", ""},
    {"UPSCALING", "Frame Synthesis Quality DLSS", "DLSS quality", dlss, "Frame Synthesis Mode", "frame_synthesis_mode_dlss"},
    {"UPSCALING", "Frame Synthesis Quality FSR", "FSR quality", fsr, "Frame Synthesis Mode", "frame_synthesis_mode_fsr"},
    {"UPSCALING", "Frame Synthesis Quality XeSS", "XeSS quality", xess, "Frame Synthesis Mode", "frame_synthesis_mode_xe_ss"},
    {"UPSCALING", "Antialiasing", "Antialiasing", antialiasing, "Frame Synthesis Mode", "frame_synthesis_mode_none"},
    {"GRAPHICS", "Texture Filtering", "Texture filtering", texture_filtering, "", ""},
    {"GRAPHICS", "Texture Quality", "Texture quality", texture_quality, "", ""},
    {"GRAPHICS", "GIBS", "Ray-traced global illumination", gibs, "", ""},
    {"GRAPHICS", "GI Grid", "Global illumination grid", gi_grid, "GIBS", "gibs_off"},
    {"GRAPHICS", "Lighting", "Lighting", lighting, "", ""},
    {"GRAPHICS", "Shadows", "Shadows", shadows, "", ""},
    {"GRAPHICS", "Mesh Quality", "Mesh quality", mesh, "", ""},
    {"GRAPHICS", "Effects Quality", "Effects quality", effects, "", ""},
    {"GRAPHICS", "Post Process", "Post processing", post_process, "", ""},
    {"GRAPHICS", "Ambient Occlusion", "Ambient occlusion", occlusion, "", ""},
    {"GRAPHICS", "Depth of Field", "Depth of field", depth_of_field, "", ""},
    {"GRAPHICS", "Motion Blur", "Motion blur", motion_blur, "", ""},
};

// The rest of the game's settings menus (ui/.../DingoUISettingAsset in its data, build
// 20260929): keys, ranges, steps and choices are the game's; the labels are ours.
constexpr Named window_mode[]{{0, "Windowed"}, {1, "Fullscreen"}, {2, "Borderless fullscreen"}};
constexpr Named hud_display[]{{0, "1"}, {1, "2"}};
constexpr Named text_language[]{{0, "English"}, {1, "French"}, {2, "German"}, {3, "Spanish"}, {4, "Italian"},
    {5, "Japanese"}, {6, "Traditional Chinese"}, {7, "Simplified Chinese"}, {8, "Korean"}, {9, "Brazilian Portuguese"}};
constexpr Named voice_language[]{{0, "English"}, {1, "French"}, {2, "German"}};
constexpr Named subtitles[]{{0, "Off"}, {1, "Narrative only"}, {2, "On"}};
constexpr Named dead_zone[]{{0, "Default"}, {1, "Wider"}, {2, "Narrow"}, {3, "Disabled"}};
constexpr Named keyframe_subject[]{{0, "Body"}, {1, "Head"}, {2, "Board"}};
constexpr Item toggle(const char* page, const char* section, const char* key, const char* label) {
    return {page, section, key, label, Kind::toggle, 0, 0, 0, {}};
}
constexpr Item whole(const char* page, const char* section, const char* key, const char* label, float least, float most, float step) {
    return {page, section, key, label, Kind::whole, least, most, step, {}};
}
constexpr Item real(const char* page, const char* section, const char* key, const char* label, float least, float most, float step) {
    return {page, section, key, label, Kind::real, least, most, step, {}};
}
constexpr Item choice(const char* page, const char* section, const char* key, const char* label, std::span<const Named> choices) {
    return {page, section, key, label, Kind::choice, 0, 0, 0, choices};
}
constexpr Item all_items[]{
    choice("GRAPHICS", "DISPLAY", "WindowedMode", "Window mode", window_mode),
    {"GRAPHICS", "DISPLAY", "Resolution", "Resolution", Kind::resolution, 0, 0, 0, {}},
    toggle("GRAPHICS", "DISPLAY", "HDR", "HDR"),
    whole("GRAPHICS", "DISPLAY", "Brightness", "Brightness", 0, 100, 5),
    real("GRAPHICS", "DISPLAY", "UIBrightness", "UI brightness", -2, 2, 1),
    choice("GRAPHICS", "DISPLAY", "HUDDisplay", "HUD display", hud_display),

    whole("AUDIO", "VOLUME", "AudioMainVolume_V2", "Main volume", 0, 100, 10),
    whole("AUDIO", "VOLUME", "StoredGlobalAudioMusic_V2", "Music volume", 0, 100, 10),
    whole("AUDIO", "VOLUME", "StoredGlobalAudioSFX_V2", "Sound effects volume", 0, 100, 10),
    whole("AUDIO", "VOLUME", "StoredGlobalAudioVO_V2", "Voice-over volume", 0, 100, 10),
    toggle("AUDIO", "VOICE CHAT", "PartyVoiceChatSetting", "Party voice chat"),
    toggle("AUDIO", "VOICE CHAT", "ProximityVoiceChatSetting", "Proximity voice chat"),
    toggle("AUDIO", "VOICE CHAT", "MyMicSetting", "Microphone on"),
    whole("AUDIO", "VOICE CHAT", "VoiceChatVolumeSetting", "Voice chat volume", 0, 100, 5),
    toggle("AUDIO", "VOICE CHAT", "SpeechToTextSetting", "Speech to text"),
    toggle("AUDIO", "VOICE CHAT", "TextToSpeechSetting", "Text to speech"),
    choice("AUDIO", "LANGUAGE AND ACCESSIBILITY", "egf_language", "Text language", text_language),
    choice("AUDIO", "LANGUAGE AND ACCESSIBILITY", "LanguageVoice", "Voice language", voice_language),
    choice("AUDIO", "LANGUAGE AND ACCESSIBILITY", "NPCSubtitles2", "Subtitles", subtitles),
    toggle("AUDIO", "LANGUAGE AND ACCESSIBILITY", "Accessibility_Party_Menu_Narration", "Party menu narration"),

    whole("CAMERA", "GENERAL", "dingocamerasensitivity_x", "Sensitivity, horizontal", 0, 100, 1),
    whole("CAMERA", "GENERAL", "dingocamerasensitivity_y", "Sensitivity, vertical", 0, 100, 1),
    toggle("CAMERA", "GENERAL", "efg_camera_resetoninput", "Camera reset"),
    toggle("CAMERA", "GENERAL", "egf_cameraenableheightinput", "On-the-fly camera"),
    whole("CAMERA", "ON BOARD", "egf_cameraonboardfovsetting", "Field of view", 0, 14, 1),
    toggle("CAMERA", "ON BOARD", "egf_OnboardFOVDistanceScale", "Field of view scaling"),
    whole("CAMERA", "ON BOARD", "egf_cameraonboardshakeintensity", "Shake intensity", 0, 10, 1),
    whole("CAMERA", "ON BOARD", "egf_onboardcamerafollowstrength_x", "Follow strength, horizontal", 0, 10, 1),
    whole("CAMERA", "ON BOARD", "egf_onboardcamerafollowstrength_y", "Follow strength, vertical", 0, 10, 1),
    whole("CAMERA", "ON BOARD", "CameraSideBiasFactor", "Side bias", 0, 10, 1),
    toggle("CAMERA", "ON BOARD", "cameradropdownpitch", "Drop-down pitch"),
    toggle("CAMERA", "ON BOARD", "egf_cameraallowautomirror", "Mirror switch, automatic"),
    toggle("CAMERA", "ON BOARD", "egf_cameraenablemirrorinput", "Mirror switch, by input"),
    whole("CAMERA", "OFF BOARD", "egf_cameraoffboardfovsetting", "Field of view", 0, 14, 1),
    toggle("CAMERA", "OFF BOARD", "egf_OffboardFOVDistanceScale", "Field of view scaling"),
    whole("CAMERA", "OFF BOARD", "egf_cameraoffboardshakeintensity", "Shake intensity", 0, 10, 1),
    whole("CAMERA", "OFF BOARD", "egf_offboardcamerafollowstrength_x", "Follow strength, horizontal", 0, 10, 1),
    whole("CAMERA", "OFF BOARD", "egf_offboardcamerafollowstrength_y", "Follow strength, vertical", 0, 10, 1),
    choice("CAMERA", "OFF BOARD", "egf_cameraoffboardfollowdeadzone", "Follow dead zone", dead_zone),
    whole("CAMERA", "OFF BOARD", "egf_cameraoffboardautofollowcooldown", "Auto-follow cooldown", 0, 10, 1),
    whole("CAMERA", "OFF BOARD", "efg_cameradefaultpitchsetting", "Default pitch", 0, 12, 1),
    whole("CAMERA", "CLIMBING", "egf_climbingcamhorizontalfollowstrength", "Follow strength, horizontal", 0, 10, 1),
    whole("CAMERA", "CLIMBING", "egf_climbingcamverticalfollowstrength", "Follow strength, vertical", 0, 10, 1),
    whole("CAMERA", "CLIMBING", "egf_climbingcamfollowcooldown", "Auto-follow cooldown", 0, 10, 1),

    toggle("CONTROLS", "CONTROLLER", "haptics", "Controller vibration"),
    toggle("CONTROLS", "CONTROLLER", "triggerforcefeedback", "Trigger force feedback"),
    toggle("CONTROLS", "CONTROLLER", "ShowControllerOnHUD", "Show controller on HUD"),
    toggle("CONTROLS", "GAMEPLAY", "CRAS_HasEnableVolumeHighlightIndicator", "Highlight indicator"),
    toggle("CONTROLS", "EXPERIMENTAL", "exp_experimentaldarkcatchandslide", "Dark catch and slide"),
    toggle("CONTROLS", "EXPERIMENTAL", "ExperimentalMegaparkCamera", "Megapark camera"),

    whole("REPLAY", "EDITOR", "replay_sensitivity_horizontal", "Sensitivity, horizontal", 0, 100, 1),
    whole("REPLAY", "EDITOR", "replay_sensitivity_vertical", "Sensitivity, vertical", 0, 100, 1),
    toggle("REPLAY", "EDITOR", "replay_invert_horizontal", "Invert horizontal"),
    toggle("REPLAY", "EDITOR", "replay_invert_vertical", "Invert vertical"),
    real("REPLAY", "EDITOR", "replay_Move_Speed", "Camera speed", 1, 20, 1),
    real("REPLAY", "EDITOR", "replay_zoom_speed", "Zoom speed", 0.1f, 4, 0.1f),
    real("REPLAY", "EDITOR", "PlaybackKeyframeSetting", "Playback speed", 0.1f, 2, 0.05f),
    real("REPLAY", "EDITOR", "MinimumKeyframeGap", "Keyframe snap range", 0, 0.25f, 0.01f),
    toggle("REPLAY", "EFFECTS", "ReplayEffect_Enabled", "Effects"),
    real("REPLAY", "EFFECTS", "ReplayEffect_Exposure_S3", "Exposure", 0, 100, 2),
    real("REPLAY", "EFFECTS", "ReplayEffect_Saturation_S3", "Saturation", 0, 100, 2),
    real("REPLAY", "EFFECTS", "ReplayEffect_ColorTemp_S3", "Colour temperature", 0, 100, 2),
    real("REPLAY", "EFFECTS", "ReplayEffect_BarrelDistortion_S3", "Barrel distortion", 0, 100, 5),
    real("REPLAY", "EFFECTS", "ReplayEffect_VignetteSize", "Vignette size", 0, 100, 2),
    real("REPLAY", "EFFECTS", "ReplayEffect_VignetteSharpness", "Vignette sharpness", 0, 100, 2),
    real("REPLAY", "EFFECTS", "ReplayEffect_VignetteOpacity", "Vignette opacity", 0, 100, 5),
    choice("REPLAY", "FIRST-PERSON KEYFRAMES", "SubjectKeyframeSetting", "Subject", keyframe_subject),
    real("REPLAY", "FIRST-PERSON KEYFRAMES", "FOVKeyframeSetting", "Field of view", 5, 140, 5),
    real("REPLAY", "FIRST-PERSON KEYFRAMES", "RollKeyframeSetting", "Roll", -180, 180, 5),
    real("REPLAY", "FIRST-PERSON KEYFRAMES", "TiltKeyframeSetting", "Tilt", -180, 180, 5),
    real("REPLAY", "FIRST-PERSON KEYFRAMES", "XOffsetKeyframe", "Offset, sideways", -10, 10, 0.1f),
    real("REPLAY", "FIRST-PERSON KEYFRAMES", "YOffsetKeyframe", "Offset, up", -10, 10, 0.1f),
};

} // namespace

Options parse_options(std::string_view text) {
    Options options;
    for (std::size_t at = 0; at <= text.size();) {
        const auto end = std::min(text.find('|', at), text.size());
        const auto part = text.substr(at, end - at);
        at = end + 1;
        if (part.empty()) continue;
        const auto split = part.find(';');
        if (split == std::string_view::npos || split == 0) fail("The game's graphics options are not in the expected form");
        options.push_back({std::string(part.substr(0, split)), std::string(part.substr(split + 1))});
    }
    return options;
}

std::string join_options(const Options& options) {
    std::string text;
    for (const auto& option : options) {
        if (option.name.empty() || (option.name + option.value).find_first_of(";|") != std::string::npos ||
            std::any_of(option.value.begin(), option.value.end(), [](unsigned char c) { return c < 0x20; }) ||
            std::any_of(option.name.begin(), option.name.end(), [](unsigned char c) { return c < 0x20; }))
            fail("A graphics option has a name or value the game's settings cannot hold");
        if (!text.empty()) text.push_back('|');
        text += option.name + ';' + option.value;
    }
    return text;
}

Values read_values(const Container& container) {
    const auto body = find_body(container);
    const auto& data = container.data;
    Values values;
    for (const auto& entry : walk(container, body).entries) {
        const auto* at = data.data() + entry.value;
        Value value;
        switch (entry.type) {
        case 6: value = *at != 0; break;
        case 8: { std::int32_t number{}; std::memcpy(&number, at, 4); value = number; break; }
        case 0xb: { float number{}; std::memcpy(&number, at, 4); value = number; break; }
        case 7: {
            std::size_t text = entry.value;
            const auto length = varint(data, text);
            value = std::string(reinterpret_cast<const char*>(data.data() + text), static_cast<std::size_t>(length) - 1);
            break;
        }
        default: continue;
        }
        if (!values.emplace(std::string(entry.name), std::move(value)).second) fail("The game's settings hold a setting twice");
    }
    return values;
}

Container with_values(const Container& container, const Values& changes) {
    const auto body = find_body(container);
    const auto walked = walk(container, body);
    const auto& data = container.data;
    constexpr unsigned types[]{6, 8, 0xb, 7};   // in the order of Value's kinds
    Bytes entries;
    std::size_t applied{};
    for (const auto& entry : walked.entries) {
        const auto change = changes.find(entry.name);
        if (change == changes.end()) {
            entries.insert(entries.end(), data.begin() + static_cast<std::ptrdiff_t>(entry.start), data.begin() + static_cast<std::ptrdiff_t>(entry.end));
            continue;
        }
        if (types[change->second.index()] != entry.type) fail("A setting is not of the kind the game saves it as");
        ++applied;
        entries.insert(entries.end(), data.begin() + static_cast<std::ptrdiff_t>(entry.start), data.begin() + static_cast<std::ptrdiff_t>(entry.value));
        if (const auto* flag = std::get_if<bool>(&change->second)) entries.push_back(*flag ? 1 : 0);
        else if (const auto* text = std::get_if<std::string>(&change->second)) {
            if (text->find('\0') != std::string::npos) fail("A setting's text cannot hold a zero byte");
            put_varint(entries, text->size() + 1);
            entries.insert(entries.end(), text->begin(), text->end());
            entries.push_back(0);
        } else {
            unsigned char raw[4];
            if (const auto* number = std::get_if<std::int32_t>(&change->second)) std::memcpy(raw, number, 4);
            else std::memcpy(raw, &std::get<float>(change->second), 4);
            entries.insert(entries.end(), raw, raw + 4);
        }
    }
    if (applied != changes.size()) fail("A setting to change is not one the game has saved");
    entries.push_back(0);
    Bytes object{data[body.offset + 16]};
    put_varint(object, entries.size());
    object.insert(object.end(), entries.begin(), entries.end());

    Container out;
    // Up to the body, its 16-byte head (length, then a time the game wrote), the new object.
    out.data.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(body.offset) + 16);
    put_u64(out.data, body.offset, object.size());
    out.data.insert(out.data.end(), object.begin(), object.end());
    out.index = container.index;
    put_u64(out.index, body.entry + index_name + 8, object.size() + 16);
    put_u64(out.index, body.entry + index_name + 24, body.offset + object.size() + 16);
    return out;
}

std::span<const Setting> graphics_options() { return all_options; }
std::span<const Item> items() { return all_items; }

std::vector<std::pair<int, int>> display_resolutions() {
    std::vector<std::pair<int, int>> out;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode); ++index) {
        const std::pair<int, int> size(static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight));
        if (std::find(out.begin(), out.end(), size) == out.end()) out.push_back(size);
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char* level_value(const Setting& setting, int level) {
    if (setting.choices.size() != std::size(quality) || level < 0 || level >= static_cast<int>(std::size(quality))) return nullptr;
    for (std::size_t i = 0; i < std::size(quality); ++i)
        if (!std::string_view(setting.choices[i].value).ends_with(std::string("_") + quality[i].value)) return nullptr;
    return setting.choices[static_cast<std::size_t>(level)].value;
}

fs::path save_root() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"Game" / L"Skate" / L"data";
}

Saved load(const fs::path& root) {
    Saved saved;
    fs::file_time_type newest{};
    std::error_code error;
    // data\<account>\<title>\<container>\{DATA, INDEX}
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end;
         it.increment(error)) {
        if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
        if (it.depth() != 2 || !it->is_directory(error)) continue;
        const auto folder = it->path();
        std::error_code missing;
        if (!fs::is_regular_file(folder / L"DATA", missing) || !fs::is_regular_file(folder / L"INDEX", missing)) continue;
        try {
            auto values = read_values({read_file(folder / L"DATA"), read_file(folder / L"INDEX")});
            // The game's settings, not another of its saves.
            const auto options = values.find(options_key);
            if (options == values.end() || !std::holds_alternative<std::string>(options->second)) continue;
            const auto written = fs::last_write_time(folder / L"DATA", missing);
            if (saved.folders.empty() || written > newest) saved.values = std::move(values), newest = written;
            saved.folders.push_back(folder);
        } catch (const std::exception&) {
            // Another of the game's saves, or a format this does not know: left alone.
        }
    }
    return saved;
}

void save(const Saved& saved, const Values& changes, const fs::path& backup) {
    if (saved.folders.empty()) fail("skate. has not saved its settings on this PC yet");
    if (changes.empty()) return;
    // Everything is read and rebuilt before any file is replaced.
    std::vector<Container> rebuilt;
    for (const auto& folder : saved.folders) {
        const Container before{read_file(folder / L"DATA"), read_file(folder / L"INDEX")};
        auto after = with_values(before, changes);
        const auto stored = read_values(after);
        for (const auto& [key, value] : changes)
            if (stored.at(key) != value) fail("The rewritten settings did not read back as they were written");
        rebuilt.push_back(std::move(after));
        if (backup.empty()) continue;
        const auto keep = backup / folder.parent_path().parent_path().filename() / folder.filename();
        std::error_code error;
        if (fs::exists(keep / L"DATA", error)) continue;
        fs::create_directories(keep, error);
        fs::copy_file(folder / L"DATA", keep / L"DATA", fs::copy_options::overwrite_existing, error);
        fs::copy_file(folder / L"INDEX", keep / L"INDEX", fs::copy_options::overwrite_existing, error);
    }
    for (std::size_t i = 0; i < rebuilt.size(); ++i) {
        // The pair is replaced back to back: INDEX gives the length of DATA's last section.
        write_file(saved.folders[i] / L"DATA", rebuilt[i].data);
        write_file(saved.folders[i] / L"INDEX", rebuilt[i].index);
    }
}

} // namespace dingosdk::launcher_game_settings
