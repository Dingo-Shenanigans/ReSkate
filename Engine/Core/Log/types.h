#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace dingosdk::logging {
enum class Level { trace, debug, info, success, warning, error, critical, off };
// Native execution context is independent of the feature/channel. For example,
// both client and server can report Player or Level messages.
enum class Context { sdk, client, server, ui, engine, filesystem, material, audio, count };
inline constexpr std::size_t context_count = static_cast<std::size_t>(Context::count);
inline constexpr std::array<std::string_view, context_count> context_names{
    "SDK", "Native(C)", "Native(S)", "Native(U)", "Native(E)", "Native(F)", "Native(M)", "Native(A)"};
inline constexpr std::string_view name(Context context) noexcept {
    const auto index = static_cast<std::size_t>(context);
    return index < context_names.size() ? context_names[index] : "SDK";
}
enum class Channel {
    runtime, level, player, skater, flow, command, launcher, hooks, graphics, input,
    ui, profile, progression, customization, world, objects, settings, music, news,
    assets, network, diagnostics, park, subworld, plugins, count
};
inline constexpr std::size_t channel_count = static_cast<std::size_t>(Channel::count);
inline constexpr std::array<std::string_view, channel_count> channel_names{
    "Runtime", "Level", "Player", "Skater", "Flow", "Console", "Launcher", "Hooks",
    "Graphics", "Input", "UI", "Profile", "Progression", "Customization", "World",
    "Objects", "Settings", "Music", "News", "Assets", "Network", "Diagnostics", "Park", "Subworld", "Plugins"};
// Defaults for existing subsystem callsites; hooks with a different native owner
// must pass their context explicitly instead of deriving it from message text.
inline constexpr Context default_context(Channel channel) noexcept {
    switch (channel) {
    case Channel::level: case Channel::player: case Channel::skater:
    case Channel::input: case Channel::profile: case Channel::progression:
    case Channel::customization: case Channel::settings: return Context::client;
    case Channel::ui: case Channel::flow: case Channel::news: return Context::ui;
    case Channel::graphics: return Context::material;
    case Channel::world: case Channel::objects: case Channel::network:
    case Channel::park: case Channel::subworld: return Context::engine;
    case Channel::assets: return Context::filesystem;
    case Channel::music: return Context::audio;
    default: return Context::sdk;
    }
}
inline constexpr std::string_view name(Channel channel) noexcept {
    const auto index = static_cast<std::size_t>(channel);
    return index < channel_names.size() ? channel_names[index] : "Runtime";
}
inline constexpr std::string_view name(Level level) noexcept {
    switch (level) {
    case Level::trace: return "TRACE";
    case Level::debug: return "DEBUG";
    case Level::info: return "INFO";
    case Level::success: return "SUCCESS";
    case Level::warning: return "WARNING";
    case Level::error: return "ERROR";
    case Level::critical: return "CRITICAL";
    default: return "OFF";
    }
}
inline constexpr std::optional<Level> parse_level(std::string_view value) noexcept {
    if (value == "trace") return Level::trace;
    if (value == "debug") return Level::debug;
    if (value == "info") return Level::info;
    if (value == "warning" || value == "warn") return Level::warning;
    if (value == "error") return Level::error;
    if (value == "critical") return Level::critical;
    if (value == "off") return Level::off;
    return std::nullopt;
}
}
