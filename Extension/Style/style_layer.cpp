#include "style_layer.h"
#include "style_live.h"
#include "style_edits.h"
#include "style_skeleton.h"
#include "style_internal.h"
#include "style_file.h"
#include "style_presets.h"
#include "Extension/Profile/local_profile.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/module_path.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Skater/no_bail.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dingosdk::style_layer {
namespace {
using namespace addr::style;
constexpr std::uint64_t let_go_ms = 2000;
constexpr std::uint64_t presets_list_ms = 2000;
constexpr std::string_view no_bail_reason = "this game build's physics state code is not the known one";

using Skeleton = frostbite::Skeleton;

struct Settings {
    std::mutex mutex;
    bool enabled{}, share{true}, dirty{true};
    std::uint64_t let_go_until{};
    std::atomic<bool> enabled_now{}, refused{};
    std::atomic<bool> auto_save_now{true}; // a lock-free copy of auto_save
    style::Style style;
    std::shared_ptr<const Skeleton> skeleton;
    style::ReadOnce<Skeleton> skeleton_read;
    std::string unavailable_reason;
    bool hooks_tried{}, hooks{};
    bool preferences_read{}, saved_enabled{}, saved_share{true};
    bool preset_read{};
    bool auto_save{true}, saved_auto_save{true};
    bool unsaved{}; // without auto save: the style differs from the file
    style::Style on_disk;
    style::Pace editor_pace{style::even_pace};
    style::History history;
    std::uint64_t save_at{};
    std::string file_issue;
    bool keep_file{}; // the saved style did not load and has no copy
    std::string preset{"default"}, saved_preset{"default"};
    std::vector<std::string> presets;
    std::uint64_t presets_at{}; // when the folder is listed next; 0 lists it at once
};
Settings &settings() { static auto *value = new Settings; return *value; }

// Runs on the read's own thread.
Skeleton read_skeleton() {
    const auto started = GetTickCount64();
    auto skeleton = style::read_game_skeleton(executable_folder());
    logging::log(logging::Level::info, logging::Channel::skater, "Style: read the skater skeleton ({} joints) in {} ms.",
                 skeleton.names.size(), GetTickCount64() - started);
    return skeleton;
}
std::shared_ptr<const Snapshot> build(const Settings &s) {
    auto result = std::make_shared<Snapshot>();
    const auto skeleton_index = [&](std::string_view name) -> std::optional<std::uint16_t> {
        const auto &names = s.skeleton->names;
        const auto found = std::ranges::find(names, name);
        if (found == names.end()) return std::nullopt;
        return static_cast<std::uint16_t>(found - names.begin());
    };
    for (std::size_t i = 0; i < style::editable_joints.size(); ++i) result->skeleton_joint[i] = skeleton_index(style::editable_joints[i]);
    auto parents = std::make_shared<std::vector<std::int32_t>>(s.skeleton->parents);
    const auto joint = [&](std::string_view name) { return skeleton_index(name).value_or(0); };
    result->rig = {{joint("AITrajectory"), joint("Deck"), joint("LeftFoot"), joint("RightFoot"), *parents}, std::move(parents)};
    for (const auto trick : style::flip_trick_ids()) result->tricks[trick] = style::trick_keys(s.style, trick);
    for (const auto &[key, degrees] : s.style.rotations)
        if (const auto joint_index = style::editable_joint(key.joint); !key.target.trick && joint_index)
            result->families[key.target.id].push_back({*joint_index, style::from_degrees(degrees[0], degrees[1], degrees[2])});
    return result;
}
void refuse(Settings &s, std::string issue) {
    if (s.unavailable_reason == issue) return;
    s.unavailable_reason = std::move(issue);
    s.refused.store(true, std::memory_order_release);
    logging::log(logging::Level::warning, logging::Channel::skater, "Style is unavailable: {}.", s.unavailable_reason);
}
// Empty when style can run.
std::string why_unavailable(const Settings &s) { return !no_bail_available() ? std::string(no_bail_reason) : s.unavailable_reason; }
std::filesystem::path presets_folder() { return profile::default_path().parent_path() / L"styles"; }
void read_style(Settings &s);
constexpr const char *profile_not_loaded = "The profile is not loaded yet.";
bool load_preferences(Settings &s) {
    if (s.preferences_read) return true;
    if (!profile_runtime::local_profile_store_ready()) return false;
    s.preferences_read = true;
    s.saved_enabled = s.enabled = profile_runtime::local_preference("Style.Enabled").value_or(false);
    s.enabled_now.store(s.enabled, std::memory_order_release);
    s.saved_share = s.share = profile_runtime::local_preference("Style.Share").value_or(true);
    s.saved_auto_save = s.auto_save = profile_runtime::local_preference("Style.AutoSave").value_or(true);
    s.auto_save_now.store(s.auto_save, std::memory_order_release);
    live().followed.share.store(s.share, std::memory_order_release);
    s.saved_preset = s.preset = style::presets::in_use_or_default(profile_runtime::local_value("Style.Preset"));
    return true;
}
bool read_preset(Settings &s) {
    if (!load_preferences(s)) return false;
    if (std::exchange(s.preset_read, true)) return true;
    if (!style::presets::exists(presets_folder(), s.preset)) s.preset = "default";
    read_style(s);
    return true;
}
void read_style(Settings &s) {
    s.unsaved = false;
    s.history.clear();
    s.keep_file = false;
    s.file_issue.clear();
    s.presets_at = 0;
    const auto read = style::presets::read(presets_folder(), s.preset);
    s.style = s.on_disk = read.style;
    s.dirty = true;
    if (!read.issue.empty()) {
        s.file_issue = read.issue;
        // This session does not write over a newer file, or over a file that has no copy.
        s.keep_file = read.keep();
        const auto *kept = read.copied ? "A copy is beside it, with .unreadable added" : "The file stays as it is";
        logging::log(logging::Level::warning, logging::Channel::skater, "Style: could not load the preset {} ({}). {}.", s.preset,
                     s.file_issue, kept);
    }
}
void save(Settings &s) {
    s.save_at = 0;
    if (s.keep_file || !s.preset_read) return;
    const auto issue = style::presets::write(presets_folder(), s.preset, s.style);
    if (issue.empty()) {
        s.file_issue.clear();
        s.on_disk = s.style;
        s.unsaved = false;
    } else {
        s.file_issue = issue;
        logging::log(logging::Level::warning, logging::Channel::skater, "Style: not saved: {}.", s.file_issue);
    }
}
void mark(Settings &s) {
    s.dirty = true;
    if (s.auto_save) s.save_at = GetTickCount64() + style::autosave_delay_ms;
    else s.unsaved = s.style != s.on_disk;
}
void changed(Settings &s, const style::Style &before, std::string what) {
    if (s.style == before) return;
    s.history.record(before, std::move(what));
    mark(s);
}
// One undo step.
template <class Apply> bool edit(std::string_view what, std::string *error, Apply &&apply) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (!read_preset(s)) {
        if (error) *error = profile_not_loaded;
        return false;
    }
    const auto before = s.style;
    if (!apply(s)) return false;
    changed(s, before, std::string(what));
    return true;
}
} // namespace
void request_enabled(bool enabled) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (s.enabled && !enabled) s.let_go_until = GetTickCount64() + let_go_ms;
    s.enabled = enabled;
    s.enabled_now.store(enabled, std::memory_order_release);
}
void request_share(bool share) {
    live().followed.share.store(share, std::memory_order_release);
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.share = share;
}
bool request_joint(style::Target target, std::string_view joint, float x, float y, float z, std::string &error) {
    const auto known = style::editable_joint(joint);
    const auto what = "change " + std::string(known ? style::editable_joints[*known] : joint);
    return edit(what, &error, [&](Settings &s) { return style::edits::set_joint(s.style, target, joint, x, y, z, error); });
}
void request_clear(std::optional<style::Target> target) {
    (void)edit(target && target->trick ? "reset trick" : "reset", nullptr, [&](Settings &s) {
        style::edits::clear(s.style, target);
        return true;
    });
}
int request_key_add(std::uint8_t trick, float time, std::string &error) {
    int added = -1;
    (void)edit("add keyframe", &error, [&](Settings &s) {
        added = style::edits::add_key(s.style, trick, time, s.editor_pace, error);
        return added >= 0;
    });
    return added;
}
bool request_key_move(std::uint8_t trick, std::uint8_t key, float time, std::string &error) {
    return edit("move keyframe", &error, [&](Settings &s) { return style::edits::move_key(s.style, trick, key, time, error); });
}
bool request_key_delete(std::uint8_t trick, std::uint8_t key, std::string &error) {
    return edit("delete keyframe", &error, [&](Settings &s) { return style::edits::delete_key(s.style, trick, key, error); });
}
bool request_key_blend_out(std::uint8_t trick, std::uint8_t key, float ms, std::string &error) {
    return edit("blend out", &error, [&](Settings &s) { return style::edits::set_blend_out(s.style, trick, key, ms, error); });
}
void request_reload() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.save_at = 0;
    if (s.preset_read) read_style(s);
    else read_preset(s);
}
namespace {
bool travel(bool back) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    auto next = back ? s.history.undo(s.style) : s.history.redo(s.style);
    if (!next) return false;
    s.style = std::move(*next);
    mark(s);
    return true;
}
} // namespace
bool request_undo() { return travel(true); }
bool request_redo() { return travel(false); }
void request_group(bool open) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.history.group(open);
}
void request_history_clear() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.history.clear();
}
void request_save() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (s.preset_read) save(s);
}
void request_auto_save(bool on) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.auto_save = on;
    s.auto_save_now.store(on, std::memory_order_release);
    if (on && s.unsaved) s.save_at = GetTickCount64() + style::autosave_delay_ms;
}
bool auto_saving() noexcept { return settings().auto_save_now.load(std::memory_order_acquire); }
namespace {
// A thread of its own: the shell can take a moment.
bool open_presets_folder(std::string &error) {
    try {
        std::thread([folder = presets_folder()] {
            std::error_code made;
            std::filesystem::create_directories(folder, made);
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (com) CoUninitialize();
        }).detach();
        return true;
    } catch (...) {
        error = "The presets folder could not be opened.";
        return false;
    }
}
std::string preset_refusal(const Settings &s, PresetAction action, const std::string &preset) {
    if (!style::preset_name(preset))
        return "A preset name has letters, digits, '-' and '_', at most 40 of them, and is not a Windows device name such as CON.";
    // Windows file names ignore case, so a name in another case is the preset in use.
    const bool in_use = console::equal(preset, s.preset);
    const bool exists = in_use || style::presets::exists(presets_folder(), preset);
    const bool must_exist = action == PresetAction::load || action == PresetAction::remove;
    if (must_exist != exists) return exists ? "A preset of that name exists." : "There is no preset of that name.";
    // Without auto save, a switch away from unsaved edits waits until they are saved or discarded. A copy takes them along.
    const bool leaves = action == PresetAction::load || action == PresetAction::create || (action == PresetAction::remove && in_use);
    if (action == PresetAction::copy && s.keep_file) return "This preset could not be read, so it cannot be copied.";
    if (s.unsaved && leaves) return "The preset has unsaved changes. Save or discard them first.";
    return {};
}
bool change_preset(Settings &s, PresetAction action, const std::string &preset, std::string &error) {
    switch (action) {
    case PresetAction::load:
        s.preset = preset;
        read_style(s);
        return true;
    case PresetAction::create:
    case PresetAction::copy:
        if (action == PresetAction::create) s.style = {};
        s.preset = preset;
        s.keep_file = false;
        s.dirty = true;
        s.history.clear();
        save(s);
        return true;
    case PresetAction::remove:
        if (!style::presets::remove(presets_folder(), preset)) {
            error = "The preset could not be deleted.";
            return false;
        }
        if (console::equal(preset, s.preset)) {
            s.preset = style::presets::after_delete(presets_folder());
            read_style(s);
        }
        return true;
    case PresetAction::open_folder:
        return true;
    }
    return true;
}
} // namespace
bool request_preset(PresetAction action, std::string_view name, std::string &error) {
    if (action == PresetAction::open_folder) return open_presets_folder(error);
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (!read_preset(s)) {
        error = profile_not_loaded;
        return false;
    }
    const std::string preset(name);
    error = preset_refusal(s, action, preset);
    if (!error.empty()) return false;
    // With auto save, edits not yet written go to the preset they were made in.
    if (s.save_at) save(s);
    if (!change_preset(s, action, preset, error)) return false;
    s.presets_at = 0;
    return true;
}
style::StyleModel model() {
    style::StyleModel result;
    auto &s = settings();
    {
        std::lock_guard lock(s.mutex);
        result.enabled = s.enabled;
        result.share = s.share;
        result.unavailable_reason = why_unavailable(s);
        result.available = result.unavailable_reason.empty();
        result.saved = !s.save_at && !s.unsaved && s.file_issue.empty();
        result.auto_save = s.auto_save;
        result.unsaved = s.unsaved;
        result.save_issue = s.file_issue;
        if (const auto *name = s.history.undo_name()) result.undo_name = *name;
        if (const auto *name = s.history.redo_name()) result.redo_name = *name;
        for (const auto &[key, degrees] : s.style.rotations)
            if (const auto joint = style::editable_joint(key.joint)) result.rotations.push_back({key.target, *joint, degrees});
        result.times = s.style.times;
        result.blend_outs = s.style.blend_outs;
        // The folder is listed again every presets_list_ms while style is on: a preset can be copied in while the game runs.
        if (const auto now = GetTickCount64(); style::presets_due(s.enabled, now, s.presets_at)) {
            s.presets_at = now + presets_list_ms;
            try {
                s.presets = style::presets::list(presets_folder(), s.preset);
            } catch (...) {
                s.presets = {s.preset};
            }
        }
        result.preset = s.preset;
        result.presets = s.presets;
    }
    return result;
}
bool enabled() noexcept { return settings().enabled_now.load(std::memory_order_acquire); }
bool available() noexcept { return no_bail_available() && !settings().refused.load(std::memory_order_acquire); }
void note_editor_pace(const style::Pace &pace) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.editor_pace = pace;
}
bool sharing() noexcept { return live().followed.share.load(std::memory_order_acquire); }
std::string status() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    read_preset(s);
    const auto &l = live();
    const auto why = why_unavailable(s);
    const std::string state = !why.empty() ? "unavailable: " + why
                              : !s.enabled ? "off"
                              : !l.followed.component.load(std::memory_order_acquire) ? "waiting for the local skater"
                              : !l.followed.active.load(std::memory_order_acquire) ? "paused during the throwdown"
                              : "on";
    const auto issue = s.file_issue.empty() ? std::string() : " Not saved: " + style::sentence(s.file_issue);
    return std::format("Style {}. Preset {}, {} joint rotation(s), {}.{}", state, s.preset, s.style.rotations.size(),
                       s.share ? "shown to other players" : "shown only to you", issue);
}

namespace {
// Stops following every rig, without a write: a rig that is not followed can be gone.
void forget_rigs(Live &l) noexcept {
    l.followed.holder.store(0, std::memory_order_release);
    l.followed.component.store(0, std::memory_order_release);
    l.followed.trick_selection.store(0, std::memory_order_release);
    l.followed.context.store(0, std::memory_order_release);
    l.pose.tracker.clear();
}
void keep_preferences(Settings &s) {
    const auto keep = [](std::string_view name, bool value, bool &saved) {
        if (value == saved) return;
        saved = value;
        profile_runtime::set_local_preference(name, value);
    };
    keep("Style.Enabled", s.enabled, s.saved_enabled);
    keep("Style.Share", s.share, s.saved_share);
    keep("Style.AutoSave", s.auto_save, s.saved_auto_save);
    if (s.preset != s.saved_preset) {
        s.saved_preset = s.preset;
        profile_runtime::set_local_values({{"Style.Preset", Json(s.preset)}});
    }
}
// With style off, the listener runs until it has restored the game's rotations, then stops. After let_go_ms it stops anyway.
void let_go(Settings &s, Live &l) {
    l.followed.active.store(false, std::memory_order_release);
    std::lock_guard pose(l.pose.mutex);
    if (l.pose.tracker.adjusted().empty() || GetTickCount64() > s.let_go_until) forget_rigs(l);
}
bool take_skeleton(Settings &s) {
    s.skeleton_read.start("the skater skeleton", read_skeleton);
    s.skeleton = s.skeleton_read.value();
    if (s.skeleton) {
        s.dirty = true;
        return true;
    }
    if (s.skeleton_read.failed()) refuse(s, "the skater skeleton could not be read");
    return false;
}
void install_hooks(Settings &s, std::uintptr_t base) {
    s.hooks_tried = true;
    std::string detail;
    s.hooks = multiplayer::install_entity_hooks(base, detail);
    if (!s.hooks) return refuse(s, "the animation hook is unavailable: " + detail);
    multiplayer::set_render_pose_writer(&on_render);
    multiplayer::set_local_pose_filter(&filter_capture);
}
bool layer_ready(Settings &s, std::uintptr_t base) {
    // No Bail verified the physics state selector before its hook, so the bytes cannot be compared here.
    if (!no_bail_available()) {
        refuse(s, std::string(no_bail_reason));
        return false;
    }
    if (!s.skeleton && !take_skeleton(s)) return false;
    if (!s.hooks_tried) install_hooks(s, base);
    return s.hooks;
}
// False when the layer does not follow the skater this tick.
bool prepare(Settings &s, Live &l, std::uintptr_t base, bool ready) {
    std::lock_guard lock(s.mutex);
    if (load_preferences(s)) {
        if (s.enabled) read_preset(s);
        keep_preferences(s);
    }
    if (s.save_at && GetTickCount64() >= s.save_at) save(s);
    if (!ready || !base) {
        std::lock_guard pose(l.pose.mutex);
        forget_rigs(l);
        return false;
    }
    if (!s.enabled) {
        let_go(s, l);
        return false;
    }
    if (!layer_ready(s, base)) return false;
    if (s.dirty) {
        s.dirty = false;
        l.followed.snapshot.store(build(s), std::memory_order_release);
    }
    return true;
}
struct LocalSkater {
    std::uintptr_t component{}, core{}, context{}, holder{};
};
std::optional<LocalSkater> local_skater(std::uintptr_t base, std::uintptr_t client) {
    const auto frame = multiplayer::capture_local(base, client, false);
    if (!frame.ready) return std::nullopt;
    const auto component = memory::peek_pointer(frame.entity, addr::style::entity_component);
    if (!component || memory::peek_pointer(component) != base + addr::engine::skater_component_vtable) return std::nullopt;
    const auto core = memory::peek_pointer(component, component_core);
    if (!core || memory::peek_pointer(core) != base + addr::no_bail::bail_core_vtable) return std::nullopt;
    const LocalSkater skater{component, core, memory::peek_pointer(core, core_context),
                             memory::peek_pointer(component, addr::style::component_pose_holder)};
    if (!skater.context || !skater.holder) return std::nullopt;
    return skater;
}
void follow(Live &l, std::uintptr_t base, const LocalSkater &skater) {
    l.followed.holder.store(skater.holder, std::memory_order_release);
    l.followed.trick_selection.store(memory::peek_pointer(skater.core, core_trick_selection), std::memory_order_release);
    l.followed.base.store(base, std::memory_order_release);
    l.followed.context.store(skater.context, std::memory_order_release);
    l.followed.active.store(!multiplayer::local_throwdown_active(), std::memory_order_release);
    if (l.followed.component.exchange(skater.component, std::memory_order_acq_rel) != skater.component)
        logging::log(logging::Level::info, logging::Channel::skater, "Style: following the local skater (component {:#x}).",
                     skater.component);
}
} // namespace
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &l = live();
    try {
        hand_over_recording(l);
        if (!prepare(settings(), l, base, ready)) return;
        if (const auto skater = local_skater(base, client)) return follow(l, base, *skater);
        l.followed.component.store(0, std::memory_order_release);
        l.followed.holder.store(0, std::memory_order_release);
    } catch (...) {}
}
} // namespace dingosdk::style_layer
