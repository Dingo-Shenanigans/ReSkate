#include "runtime_internal.h"
#include "Engine/Core/Console/console_core.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <cstdio>
#include <memory>

namespace dingosdk::runtime::detail {
namespace {
const char* command_parse_error(dingosdk::CommandParseError error) {
    using Error = dingosdk::CommandParseError;
    switch (error) {
    case Error::input_too_long: return "Command is too long.";
    case Error::invalid_character: return "Command contains an unsupported character.";
    case Error::too_many_arguments: return "Command has too many arguments.";
    case Error::unterminated_quote: return "Command has an unterminated quote.";
    case Error::dangling_escape: return "Command ends with an incomplete escape.";
    default: return "Command syntax is invalid.";
    }
}
}
void read_model(void*, dingosdk::overlay::Model& output) {
    auto& r = runtime();
    // The menu or console is up: keep engine settings fresh on the game thread.
    r.named_settings_wanted_until.store(GetTickCount64() + 1000, std::memory_order_relaxed);
    // `output` is the copy the overlay already holds (or a new one). Copy only
    // the parts that changed since it was taken: the game thread takes this
    // lock several times every frame and must not wait on a full model copy.
    const auto held = output.revisions;
    auto debug = std::move(output.debug);
    auto offline = std::move(output.offline);
    auto settings = std::move(output.engine_settings);
    std::shared_ptr<const NamedSettings> fresh_settings;
    {
        std::lock_guard lock(r.mutex);
        if (held.model != r.model_revision) {
            output = r.model;
        } else {
            // The held copy has the adjustments below applied: start again from the source.
            output.can_queue_load = r.model.can_queue_load;
            output.load_block_reason = r.model.load_block_reason;
        }
        if (held.debug != r.debug_revision) debug = r.debug_model;
        if (held.offline != r.offline_revision) offline = r.offline_model;
        if (held.engine_settings != r.named_settings_revision) fresh_settings = r.named_settings;
        output.revisions = {r.model_revision, r.debug_revision, r.offline_revision, r.named_settings_revision};
        if (!r.requests.idle()) {
            output.can_queue_load = false;
            output.load_block_reason = "Waiting for queued game work or a level transition.";
        }
    }
    output.debug = std::move(debug);
    output.offline = std::move(offline);
    // Thousands of rows: copied outside the lock, and only after they changed.
    if (held.engine_settings != output.revisions.engine_settings)
        settings = fresh_settings ? *fresh_settings : NamedSettings{};
    output.engine_settings = std::move(settings);
    output.console_log.clear();
    output.menu_scale = dingosdk::profile_runtime::local_menu_scale();
    output.menu_scale_fullscreen = dingosdk::profile_runtime::local_menu_scale(true);
    output.menu_fullscreen = dingosdk::profile_runtime::local_menu_fullscreen();
    output.steam_offline = dingosdk::launcher::offline_mode();
    output.multiplayer=dingosdk::multiplayer::model();
    if (dingosdk::multiplayer_controls_level(output.multiplayer)) {
        output.can_queue_load = false;
        output.load_block_reason = "Only the lobby host can change levels.";
    }
    // The overlay retains its own bounded history across hide/reopen and
    // graphics recreation. Deliver only new lines instead of copying all 512
    // retained strings every rendered menu frame. Gaps retain their sequence
    // numbers so the existing UI can report evicted lines when reopened.
    if (dingosdk::logging::latest_sequence() != r.console_delivered_sequence) {
        output.console_log = dingosdk::logging::snapshot_after(r.console_delivered_sequence);
        if (!output.console_log.empty())
            r.console_delivered_sequence = output.console_log.back().sequence;
    }
    dingosdk::overlay::Status status;
    DingoSDKOverlayGetStatus(&status);
    if (status.ready && status.rendered_frames && !r.overlay_confirmed.exchange(true)) {
        const auto keys = dingosdk::launcher::overlay_keys();
        activity_line(dingosdk::ConsoleSource::runtime, "Overlay ready. Press " + dingosdk::launcher::key_name(keys.console) +
            " for the console or " + dingosdk::launcher::key_name(keys.menu) + " for the menu.");
        record("{\"event\":\"overlay_rendering\",\"rendered_frames\":" + std::to_string(status.rendered_frames) + "}");
    }
}
void read_native_menu_model(void*, dingosdk::overlay::Model& output) {
    auto& r = runtime();
    {
        std::lock_guard lock(r.mutex);
        output = r.model;
        output.debug = r.debug_model;
        output.offline = r.offline_model; // Board wear and other feature toggles.
        if (!r.requests.idle()) {
            output.can_queue_load = false;
            output.load_block_reason = "Waiting for queued game work or a level transition.";
        }
    }
    output.multiplayer = dingosdk::multiplayer::model();
    if (dingosdk::multiplayer_controls_level(output.multiplayer)) {
        output.can_queue_load = false;
        output.load_block_reason = "Only the lobby host can change levels.";
    }
    // Native UI has no console reader. Do not advance the overlay log cursor.
    output.console_log.clear();
}
bool queue_debug(void*, const dingosdk::overlay::DebugRequest& request, char* result, std::size_t size) {
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (!r.requests.enqueue(request, request_context(r), GetCurrentThreadId())) {
        if (size) std::snprintf(result, size, "Debug request unavailable while loading or queue is full.");
        return false;
    }
    if (size) result[0] = 0; // Accepted: the change speaks for itself.
    return true;
}
bool queue_offline_feature(void*, const dingosdk::overlay::OfflineFeatureRequest& request,
                           char* result, std::size_t size) {
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    const auto context = request_context(r);
    if (!r.requests.enqueue(request, context, GetCurrentThreadId())) {
        dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
            "Offline feature %d -> %d rejected (tick %d, healthy %d, offline %d, loading %d).",
            static_cast<int>(request.group), request.enabled, context.tick_ready, context.healthy,
            context.offline_ready, r.requests.loading());
        if (size) std::snprintf(result, size, "Offline setting unavailable while loading, disconnected, or the queue is full.");
        return false;
    }
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
        "Offline feature %d -> %d queued.", static_cast<int>(request.group), request.enabled);
    if (size) result[0] = 0; // Accepted: the change speaks for itself.
    return true;
}
bool queue_multiplayer_command(const char* action, const char* argument, const char* password,
                               char* result, std::size_t size) {
    const bool ok = action && argument && password && dingosdk::multiplayer::queue_command(action, argument, password);
    if (size) std::snprintf(result, size, "%s", ok ? "" : "Multiplayer request unavailable or too long.");
    return ok;
}
bool queue_console_command(void*, const char* command, char* result, std::size_t size) {
    if (!command) {
        if (size) std::snprintf(result, size, "Command is missing.");
        return false;
    }
    std::size_t length{};
    while (length <= dingosdk::console_max_command_bytes && command[length]) ++length;
    if (length > dingosdk::console_max_command_bytes) {
        if (size) std::snprintf(result, size, "Command is too long.");
        return false;
    }
    const auto parsed = dingosdk::parse_console_command(std::string_view(command, length));
    if (!parsed) {
        if (size) std::snprintf(result, size, "%s", command_parse_error(parsed.error));
        return false;
    }
    if (parsed.arguments.empty()) {
        if (size) std::snprintf(result, size, "Enter a command. Try 'help'.");
        return false;
    }
    auto& r = runtime();
    // Commands can show engine settings: keep their values fresh for a while.
    r.named_settings_wanted_until.store(GetTickCount64() + 10000, std::memory_order_relaxed);
    std::lock_guard lock(r.mutex);
    if (!r.requests.enqueue(ConsoleRequest{std::string(command, length)}, request_context(r), GetCurrentThreadId())) {
        if (size) std::snprintf(result, size,
            "Console command unavailable until the game update thread is ready.");
        return false;
    }
    if (size) result[0] = 0; // Accepted: the change speaks for itself.
    return true;
}
}
