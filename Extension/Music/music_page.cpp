#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include "audio_dsp.h"
#include "external_audio.h"
#include "external_player.h"
#include "music_inject.h"
#include "spotify_dashboard.h"
#include "spotify_device.h"
#include "spotify_station.h"
#include "spotify_web.h"
#include "youtube_music.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <string>
#include <vector>

// The MUSIC page of the menu: what is playing from the music sources (Spotify, YouTube Music,
// the player's files, radio, another application) with play, pause and next, what is lined up,
// and how it sounds (loudness, levelling, the equaliser). Setting a source up is done in the
// browser (spotify_dashboard.h), which this opens. Presentation thread: it reads what the
// sources say of themselves a few times a second, and asks of them what the setup page does.
namespace dingosdk::overlay::menu {
namespace {
struct Page {
    int tab{};
    double read_at{-1};
    youtube_music::State streams;
    std::vector<spotify_web::Queued> spotify;
    std::string title, artist, source;
    bool available{}, playing{}, station{};
    std::array<char, 96> find{};
    // The sliders, as they are while one is held.
    float gain{100};
    std::array<float, audio_dsp::bands> bands{};
    bool editing{};
    double pressed{-1};
};
Page& page() {
    static auto* value = new Page;
    return *value;
}

void read(Page& p) {
    if (p.read_at >= 0 && ImGui::GetTime() - p.read_at < 0.4) return;
    p.read_at = ImGui::GetTime();
    p.streams = youtube_music::state();
    p.station = spotify_station::active();
    const auto now = external_player::now();
    p.available = now.available && p.station;
    p.title = now.title;
    p.artist = now.artist;
    p.playing = now.playing;
    const bool streams = youtube_music::on(), others = external_audio::on();
    p.source = !p.station ? "" : others ? "EXTERNAL AUDIO" : !streams ? "SPOTIFY"
        : p.streams.playing_list < p.streams.playlists.size() && p.streams.playlists[p.streams.playing_list].kind == youtube_music::Kind::files ? "LOCAL FILES"
        : p.streams.playing_list < p.streams.playlists.size() && p.streams.playlists[p.streams.playing_list].kind == youtube_music::Kind::radio ? "ONLINE RADIO" : "YOUTUBE MUSIC";
    p.spotify.clear();
    if (p.station && !streams && !others) {
        spotify_web::want_queue();
        p.spotify = spotify_web::queue();
    }
    if (!p.editing) {
        p.gain = music_inject::gain() * 100.0f;
        for (int i = 0; i < audio_dsp::bands; ++i) p.bands[static_cast<std::size_t>(i)] = audio_dsp::band(i);
    }
}

bool found(const std::array<char, 96>& find, const std::string& artist, const std::string& title) {
    if (!find[0]) return true;
    const auto lower = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    return lower(title + " " + artist).find(lower(find.data())) != std::string::npos;
}

void player_tab(SkateMenu& menu, Page& p) {
    begin_card(menu, "music-now", "NOW PLAYING");
    if (!p.available || p.title.empty()) {
        note(p.station ? "Waiting for the first song." : "Nothing is playing. Pick a tile in the game's music screen: Spotify, YouTube Music, Local Files, Online Radio or External Audio.");
    } else {
        ImGui::PushFont(menu.heading);
        ImGui::TextUnformatted(p.title.c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
        ImGui::TextUnformatted(p.artist.c_str());
        ImGui::PopStyleColor();
        if (!p.source.empty()) tag(menu, p.source.c_str(), skate_theme::blue);
        if (p.streams.loading) {
            ImGui::SameLine(0, px(8));
            tag(menu, "LOADING", skate_theme::warning);
        }
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    using external_player::Command;
    if (console_style()) {
        const int pressed = button_row(menu, "music-buttons", {{"PREVIOUS", p.station}, {p.playing ? "PAUSE" : "PLAY", p.station, true}, {"NEXT", p.station}});
        if (pressed == 0) external_player::send(Command::previous);
        if (pressed == 1) external_player::send(Command::toggle), p.read_at = -1;
        if (pressed == 2) external_player::send(Command::next);
    } else {
    const float gap = px(6), width = (ImGui::GetContentRegionAvail().x - gap * 2) / 3;
    ImGui::BeginDisabled(!p.station);
    if (ImGui::Button("PREVIOUS", ImVec2(width, px(36)))) external_player::send(Command::previous);
    ImGui::SameLine(0, gap);
    skate_theme::push_primary_button();
    if (ImGui::Button(p.playing ? "PAUSE" : "PLAY", ImVec2(width, px(36)))) {
        external_player::send(Command::toggle);
        p.read_at = -1;
    }
    skate_theme::pop_primary_button();
    ImGui::SameLine(0, gap);
    if (ImGui::Button("NEXT", ImVec2(width, px(36)))) external_player::send(Command::next);
    ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    bool notices = youtube_music::notices();
    if (toggle_row(menu, "Show \"Loading song\" in the corner", "The notice in the top left while a YouTube song or a radio station is on its way.", notices)) {
        youtube_music::set_notices(notices);
        spotify_dashboard::save_settings();
    }
    end_card();

    // What is lined up: the playlist that is playing, or what Spotify says comes next.
    const auto& s = p.streams;
    const bool lined_up = s.playing && s.playing_list < s.playlists.size();
    begin_card(menu, "music-queue", "QUEUE", lined_up ? s.playlists[s.playing_list].name.c_str() : !p.spotify.empty() ? "Up next on Spotify" : nullptr);
    if (lined_up && console_style()) {
        const auto& songs = s.playlists[s.playing_list].songs;
        const bool held = s.loading || (p.pressed >= 0 && ImGui::GetTime() - p.pressed < 1.5);
        for (std::size_t i = 0; i < songs.size(); ++i) {
            const bool current = i == s.playing_song;
            if (list_row(menu, std::to_string(i).c_str(), std::format("{:>3}   {}", i + 1, songs[i].title), songs[i].artist, current, !held) && !current) {
                youtube_music::play_at(s.playing_list, i);
                p.pressed = ImGui::GetTime();
                p.read_at = -1;
            }
            if (ImGui::IsItemFocused()) set_console_hint(current ? "Playing now." : "A plays this song.");
        }
    } else if (lined_up) {
        const auto& songs = s.playlists[s.playing_list].songs;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##music-find", "Find a song or artist", p.find.data(), p.find.size());
        ImGui::BeginChild("music-songs", ImVec2(0, std::max(px(160), ImGui::GetContentRegionAvail().y - px(330))), ImGuiChildFlags_NavFlattened);
        // (One press at a time: a song that is loading is not begun again under more presses.)
        const bool held = s.loading || (p.pressed >= 0 && ImGui::GetTime() - p.pressed < 1.5);
        for (std::size_t i = 0; i < songs.size(); ++i) {
            if (!found(p.find, songs[i].artist, songs[i].title)) continue;
            ImGui::PushID(static_cast<int>(i));
            const bool current = i == s.playing_song;
            if (current) ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::blue_hover);
            if (ImGui::Selectable(std::format("{:>3}   {}", i + 1, songs[i].title).c_str(), current, held ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None) && !current) {
                youtube_music::play_at(s.playing_list, i);
                p.pressed = ImGui::GetTime();
                p.read_at = -1;
            }
            if (current) ImGui::PopStyleColor();
            ImGui::SameLine(0, px(12));
            ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
            ImGui::TextUnformatted(songs[i].artist.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    } else if (!p.spotify.empty()) {
        ImGui::BeginChild("music-spotify", ImVec2(0, std::max(px(160), ImGui::GetContentRegionAvail().y - px(330))), ImGuiChildFlags_NavFlattened);
        for (std::size_t i = 0; i < p.spotify.size(); ++i) {
            ImGui::PushStyleColor(ImGuiCol_Text, i ? skate_theme::white : skate_theme::blue_hover);
            ImGui::TextUnformatted(std::format("{:>3}   {}", i + 1, p.spotify[i].title).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, px(12));
            ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
            ImGui::TextUnformatted(p.spotify[i].artist.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    } else {
        note(p.source == "SPOTIFY" ? "To see Spotify's queue, sign in under \"Show your own playlists\" on the setup page (SOURCES tab)." : "Nothing is lined up.");
    }
    end_card();
}

void sound_tab(SkateMenu& menu, Page& p) {
    bool editing{}, changed{};
    begin_card(menu, "music-loudness", "LOUDNESS");
    field(menu, "Music loudness", "How loud the music you play through ReSkate is in the game.");
    if (::dingosdk::overlay::menu::slider_float("##music-gain", &p.gain, 0.0f, 400.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) music_inject::set_gain(p.gain / 100.0f);
    editing |= ImGui::IsItemActive();
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    bool levelling = audio_dsp::levelling();
    if (toggle_row(menu, "Keep every song at the same loudness", "Turns quiet songs up and loud ones down.", levelling)) {
        audio_dsp::set_levelling(levelling);
        changed = true;
    }
    bool speakers = music_inject::mode() != 3;
    if (toggle_row(menu, "Speakers in the world play it too", "Speakers around the map play what you are playing.", speakers)) {
        music_inject::set_mode(speakers ? 1 : 3);
        changed = true;
    }
    end_card();

    begin_card(menu, "music-equaliser", "EQUALISER", "Up for more of that part of the sound, down for less.");
    static constexpr std::array<const char*, audio_dsp::bands> names{"BASS", "LOW MID", "MID", "HIGH MID", "TREBLE"}, where{"60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"};
    struct Preset {
        const char* name;
        std::array<float, audio_dsp::bands> bands;
    };
    static constexpr std::array<Preset, 5> presets{{{"FLAT", {0, 0, 0, 0, 0}}, {"BASS BOOST", {6, 3, 0, 0, 0}}, {"LOUD", {5, 2, -1, 2, 4}}, {"VOCAL", {-2, 0, 3, 3, 0}}, {"BRIGHT", {0, 0, 0, 3, 5}}}};
    if (console_style()) {
        for (int i = 0; i < audio_dsp::bands; ++i) {
            const auto at = static_cast<std::size_t>(i);
            field(menu, std::format("{}  ({})", names[at], where[at]).c_str(), "Left / right a decibel at a time.");
            ImGui::PushID(i);
            set_console_step(1.0);
            if (slider_float("##band", &p.bands[at], -audio_dsp::most_decibels, audio_dsp::most_decibels, "%+.0f dB", ImGuiSliderFlags_AlwaysClamp)) {
                p.bands[at] = std::round(p.bands[at]);
                audio_dsp::set_band(i, p.bands[at]);
            }
            editing |= ImGui::IsItemActive();
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        const int pressed = button_row(menu, "eq-presets", {{presets[0].name, true, presets[0].bands == p.bands}, {presets[1].name, true, presets[1].bands == p.bands},
            {presets[2].name, true, presets[2].bands == p.bands}, {presets[3].name, true, presets[3].bands == p.bands}, {presets[4].name, true, presets[4].bands == p.bands}});
        if (pressed >= 0) {
            p.bands = presets[static_cast<std::size_t>(pressed)].bands;
            for (int band = 0; band < audio_dsp::bands; ++band) audio_dsp::set_band(band, p.bands[static_cast<std::size_t>(band)]);
            changed = true;
        }
        note("The loudness, levelling and equaliser only change music played through ReSkate, not the game's own.");
        end_card();
        p.editing = editing;
        if (changed) spotify_dashboard::save_settings();
        return;
    }
    // Each band is a column: how much it is set to, a narrow track with a line where 0 dB is
    // and a filled bar from that line to the handle, then its name and where it sits.
    const float gap = px(10), column = std::min(px(96), (ImGui::GetContentRegionAvail().x - gap * (audio_dsp::bands - 1)) / audio_dsp::bands);
    const float track = px(34), tall = px(150);
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto centred = [&](ImFont* font, float size, float x, float y, ImU32 colour, const char* text) {
        const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
        draw->AddText(font, size, ImVec2(x + (column - extent.x) * .5f, y), colour, text);
    };
    for (int i = 0; i < audio_dsp::bands; ++i) {
        const auto at = static_cast<std::size_t>(i);
        const float x = origin.x + (column + gap) * static_cast<float>(i);
        const float value = p.bands[at];
        centred(menu.bold, px(15), x, origin.y, std::fabs(value) < 0.5f ? skate_theme::grey_text : skate_theme::white, std::format("{:+.0f} dB", value).c_str());
        const ImVec2 top(x + (column - track) * .5f, origin.y + px(24));
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(top);
        // (The slider itself is not drawn: only its handle, over what is drawn here.)
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
        if (ImGui::VSliderFloat("##band", ImVec2(track, tall), &p.bands[at], -audio_dsp::most_decibels, audio_dsp::most_decibels, "", ImGuiSliderFlags_AlwaysClamp)) {
            p.bands[at] = std::round(p.bands[at]);
            audio_dsp::set_band(i, p.bands[at]);
        }
        ImGui::PopStyleColor(5);
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        editing |= ImGui::IsItemActive();
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) draw->AddRect(top, ImVec2(top.x + track, top.y + tall), skate_theme::white, 0, 0, px(2));
        ImGui::PopID();
        const float middle = top.y + tall * .5f, rail = px(6), handle = px(8);
        const float level = middle - p.bands[at] / audio_dsp::most_decibels * (tall * .5f - handle * .5f);
        const float centre = top.x + track * .5f;
        draw->AddRectFilled(ImVec2(centre - rail * .5f, top.y), ImVec2(centre + rail * .5f, top.y + tall), skate_theme::tile_light);
        draw->AddRectFilled(ImVec2(centre - rail * .5f, std::min(level, middle)), ImVec2(centre + rail * .5f, std::max(level, middle)), skate_theme::blue);
        draw->AddRectFilled(ImVec2(top.x, middle - px(1)), ImVec2(top.x + track, middle + px(1)), skate_theme::grey_text);
        draw->AddRectFilled(ImVec2(top.x, level - handle * .5f), ImVec2(top.x + track, level + handle * .5f), hot ? skate_theme::blue_hover : skate_theme::white);
        centred(menu.bold, px(13), x, top.y + tall + px(8), skate_theme::white, names[at]);
        centred(menu.body, px(13), x, top.y + tall + px(26), skate_theme::grey_text, where[at]);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2((column + gap) * audio_dsp::bands - gap, px(24) + tall + px(46)));
    ImGui::Dummy(ImVec2(0, px(6)));
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (i) ImGui::SameLine(0, px(6));
        const bool on = presets[i].bands == p.bands;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
        if (ImGui::Button(presets[i].name)) {
            p.bands = presets[i].bands;
            for (int band = 0; band < audio_dsp::bands; ++band) audio_dsp::set_band(band, p.bands[static_cast<std::size_t>(band)]);
            changed = true;
        }
        if (on) ImGui::PopStyleColor();
    }
    note("The loudness, levelling and equaliser only change music played through ReSkate, not the game's own.");
    end_card();
    p.editing = editing;
    if (changed) spotify_dashboard::save_settings();
}

void sources_tab(SkateMenu& menu, Page& p) {
    begin_card(menu, "music-setup", "SET UP", "Sign in, add playlists, folders and stations.");
    note("Sources are set up on a page in your browser. It only works on this PC, while the game is running.");
    if (primary_button(menu, "OPEN THE SETUP PAGE IN YOUR BROWSER"))
        feedback(menu, spotify_dashboard::open() ? "Opened the setup page in your browser. Alt-Tab to it." : "The setup page could not be opened.");
    end_card();

    using youtube_music::Kind;
    const auto count = [&](Kind kind) { return std::count_if(p.streams.playlists.begin(), p.streams.playlists.end(), [&](const youtube_music::Playlist& playlist) { return playlist.kind == kind; }); };
    begin_card(menu, "music-sources", "SOURCES");
    info(menu, "Spotify", spotify_device::connected() ? "Connected" : spotify_device::in_use() ? "Remembered, connects by itself" : "Pick \"ReSkate\" in Spotify's device list");
    info(menu, "YouTube Music", !p.streams.tools ? "Not set up" : std::format("{} playlist(s)", count(Kind::youtube)));
    info(menu, "Local files", count(Kind::files) ? std::format("{} folder(s)", count(Kind::files)) : std::string("No folder added"));
    info(menu, "Online radio", count(Kind::radio) ? std::format("{} station(s)", count(Kind::radio)) : std::string("No station added"));
    info(menu, "External audio", external_audio::app().empty() ? std::string("No app chosen") : external_audio::app());
    if (!p.streams.problem.empty()) warn(p.streams.problem.c_str());
    end_card();

    if (count(Kind::files)) {
        begin_card(menu, "music-folders", "LOCAL FOLDERS", "Rescan after adding files to one.");
        for (std::size_t i = 0; i < p.streams.playlists.size(); ++i) {
            const auto& playlist = p.streams.playlists[i];
            if (playlist.kind != Kind::files) continue;
            ImGui::PushID(static_cast<int>(i));
            if (console_style()) {
                const auto label = std::format("RESCAN {}  ({} songs)", playlist.name, playlist.songs.size());
                if (button_row(menu, "rescan", {{label.c_str(), !p.streams.busy}}) == 0) {
                    youtube_music::add_folder(playlist.url);
                    feedback(menu, "Reading the folder again.");
                    p.read_at = -1;
                }
                ImGui::PopID();
                continue;
            }
            ImGui::BeginDisabled(p.streams.busy);
            if (ImGui::Button("RESCAN")) {
                youtube_music::add_folder(playlist.url);
                feedback(menu, "Reading the folder again.");
                p.read_at = -1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, px(10));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(std::format("{}  ({} songs)", playlist.name, playlist.songs.size()).c_str());
            ImGui::PopID();
        }
        end_card();
    }
}
}

void music_page(SkateMenu& menu, const Model&, const CallbacksV3&) {
    auto& p = page();
    read(p);
    category_tabs(menu, p.tab, {"PLAYER", "SOUND", "SOURCES"}, "music-tabs");
    ImGui::PushID(p.tab);
    ImGui::BeginChild("music-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    if (p.tab == 0) {
        player_tab(menu, p);
        music_playback_card(menu);
    }
    else if (p.tab == 1) sound_tab(menu, p);
    else sources_tab(menu, p);
    ImGui::EndChild();
    ImGui::PopID();
}
}
