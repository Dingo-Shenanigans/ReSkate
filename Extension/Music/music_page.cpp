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

// Settings > MUSIC in the menu: what is playing from the music sources (Spotify, YouTube Music,
// the player's files, radio, another application) with play, pause and next, what is lined up,
// and how it sounds (loudness, levelling, the equaliser). Setting a source up is done in the
// browser (spotify_dashboard.h), which this opens. Presentation thread: it reads what the
// sources say of themselves a few times a second, and asks of them what the setup page does.
namespace dingosdk::overlay::menu {
namespace {
struct Page {
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

void player_cards(SkateMenu& menu, Page& p) {
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
    const int pressed = button_row(menu, "music-buttons", {{"PREVIOUS", p.station}, {p.playing ? "PAUSE" : "PLAY", p.station, true}, {"NEXT", p.station}});
    if (pressed == 0) external_player::send(Command::previous);
    if (pressed == 1) external_player::send(Command::toggle), p.read_at = -1;
    if (pressed == 2) external_player::send(Command::next);
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
    if (lined_up) {
        // A row a song, the one playing in blue; A plays one. The box finds a song or an artist, for a
        // keyboard; a controller passes over it. A long playlist (a big folder) goes through a clipper.
        const auto& songs = s.playlists[s.playing_list].songs;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##music-find", "Find a song or artist", p.find.data(), p.find.size());
        std::vector<std::size_t> shown;
        for (std::size_t i = 0; i < songs.size(); ++i)
            if (found(p.find, songs[i].artist, songs[i].title)) shown.push_back(i);
        // (One press at a time: a song that is loading is not begun again under more presses.)
        const bool held = s.loading || (p.pressed >= 0 && ImGui::GetTime() - p.pressed < 1.5);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto i = shown[static_cast<std::size_t>(row)];
                const bool current = i == s.playing_song;
                if (list_row(menu, std::to_string(i).c_str(), std::format("{:>3}   {}", i + 1, songs[i].title), songs[i].artist, current, !held) && !current) {
                    youtube_music::play_at(s.playing_list, i);
                    p.pressed = ImGui::GetTime();
                    p.read_at = -1;
                }
                if (ImGui::IsItemFocused()) set_console_hint(current ? "Playing now." : "A plays this song.");
            }
        }
        if (shown.empty()) note("No song or artist matches that.");
    } else if (!p.spotify.empty()) {
        // What Spotify says comes next, the first playing now. (Spotify plays it: nothing here to press.)
        for (std::size_t i = 0; i < p.spotify.size(); ++i) {
            ImGui::PushStyleColor(ImGuiCol_Text, i ? skate_theme::white : skate_theme::blue_hover);
            ImGui::TextUnformatted(std::format("{:>3}   {}", i + 1, p.spotify[i].title).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, px(12));
            ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
            ImGui::TextUnformatted(p.spotify[i].artist.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        note(p.source == "SPOTIFY" ? "To see Spotify's queue, sign in under \"Show your own playlists\" on the setup page (SOURCES, below)." : "Nothing is lined up.");
    }
    end_card();
}

void sound_cards(SkateMenu& menu, Page& p) {
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
}

void sources_cards(SkateMenu& menu, Page& p) {
    begin_card(menu, "music-setup", "SET UP", "Sign in, add playlists, folders and stations.");
    note("Sources are set up on a page in your browser. It only works on this PC, while the game is running.");
    if (primary_button(menu, "OPEN THE SETUP PAGE IN YOUR BROWSER"))
        feedback(menu, spotify_dashboard::open() ? "Opened the setup page in your browser. Alt-Tab to it." : "The setup page could not be opened.");
    end_card();

    using youtube_music::Kind;
    const auto count = [&](Kind kind) { return std::count_if(p.streams.playlists.begin(), p.streams.playlists.end(), [&](const youtube_music::Playlist& playlist) { return playlist.kind == kind; }); };
    begin_card(menu, "music-sources", "STATUS");
    info(menu, "Spotify", spotify_device::connected() ? "Connected" : spotify_device::in_use() ? "Remembered, connects by itself" : "Pick \"ReSkate\" in Spotify's device list");
    info(menu, "YouTube Music", !p.streams.tools ? "Not set up" : std::format("{} playlist(s)", count(Kind::youtube)));
    info(menu, "Local files", count(Kind::files) ? std::format("{} folder(s)", count(Kind::files)) : std::string("No folder added"));
    info(menu, "Online radio", count(Kind::radio) ? std::format("{} station(s)", count(Kind::radio)) : std::string("No station added"));
    info(menu, "External audio", external_audio::app().empty() ? std::string("No app chosen") : external_audio::app());
    if (!p.streams.problem.empty()) warn(p.streams.problem.c_str());
    end_card();

    if (count(Kind::files)) {
        begin_card(menu, "music-folders", "LOCAL FOLDERS", "A rescans one after you add files.");
        for (std::size_t i = 0; i < p.streams.playlists.size(); ++i) {
            const auto& playlist = p.streams.playlists[i];
            if (playlist.kind != Kind::files) continue;
            // A row a folder, its songs at the right end; A reads it again.
            ImGui::PushID(static_cast<int>(i));
            if (list_row(menu, "##folder", playlist.name, std::format("{} songs", playlist.songs.size()), false, !p.streams.busy)) {
                youtube_music::add_folder(playlist.url);
                feedback(menu, "Reading the folder again.");
                p.read_at = -1;
            }
            if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) set_console_hint("A rescans this folder.");
            ImGui::PopID();
        }
        end_card();
    }
}
}

// One tab of Settings, which scrolls it: the player, the sound and the sources, each under its heading.
void music_page(SkateMenu& menu, const Model&, const CallbacksV3&) {
    auto& p = page();
    read(p);
    section(menu, "PLAYER");
    player_cards(menu, p);
    music_playback_card(menu);
    section(menu, "SOUND");
    sound_cards(menu, p);
    section(menu, "SOURCES");
    sources_cards(menu, p);
}
}
