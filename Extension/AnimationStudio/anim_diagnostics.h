#pragma once
#include "anim_core.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <string>

namespace dingosdk::anim_studio {
inline std::atomic<bool> diagnostics_capture{true};
struct DiagnosticSample {
    double at{};
    std::uint64_t generation{}, sequence{}, failures{}, skater_publications{}, board_publications{};
    std::optional<std::uint32_t> physics;
    std::optional<bool> live_switch, takeoff_switch, recorded_switch;
    bool goofy{}, takeoff_goofy{}, recorded_goofy{}, enabled{}, armed{}, retarget{}, mirror{};
    bool reverse_facing{};
    float clip_time{-1}, weight{}, board_weight{};
    double skater_age{-1}, board_age{-1};
    double render_at{-1}, render_board_at{-1};
    std::string trick, preset;
    std::optional<Joint> native_board, desired_board, render_board;
    std::vector<Joint> native, desired, render_native;
    std::string observed_phase, entry, source_kind;
    std::uint64_t episode{}, render_edit_sequence{}, render_presentation{}, board_edit_sequence{}, board_presentation{};
    std::vector<Joint> render_edited;
    double render_edited_at{-1}, render_edited_board_at{-1};
    std::optional<Joint> render_edited_board;
    std::vector<Joint> source, board_rig;
    double board_rig_at{-1};
    std::optional<Joint> source_board;
};
struct DiagnosticBuffer {
    static constexpr std::size_t capacity = 4096;
    static constexpr double duration = 15.0, pose_interval = 0.1;
    std::deque<DiagnosticSample> samples;
    double last_pose{-1};
    bool pose_due(double now) const noexcept { return last_pose < 0 || now - last_pose >= pose_interval; }
    void append(DiagnosticSample sample) {
        if (!std::isfinite(sample.at)) return;
        if (!samples.empty() && sample.at < samples.back().at) { samples.clear(); last_pose = -1; }
        if (!sample.native.empty() || !sample.desired.empty() || !sample.render_native.empty() || !sample.render_edited.empty()) {
            if (pose_due(sample.at)) last_pose = sample.at;
            else {
                std::vector<Joint>{}.swap(sample.native); std::vector<Joint>{}.swap(sample.desired);
                std::vector<Joint>{}.swap(sample.render_native); std::vector<Joint>{}.swap(sample.render_edited);
                std::vector<Joint>{}.swap(sample.source); std::vector<Joint>{}.swap(sample.board_rig);
            }
        }
        samples.push_back(std::move(sample));
        while (samples.size() > capacity || (!samples.empty() && samples.back().at - samples.front().at > duration))
            samples.pop_front();
    }
};
inline std::string diagnostic_csv(std::string_view text) {
    std::string out = "\"";
    for (char c : text) { if (c == '"') out += '"'; out += c; }
    return out + '"';
}
inline Json write_diagnostic_history(const DiagnosticBuffer& history, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::ofstream timeline(folder / "timeline.csv"), poses(folder / "poses.csv"), board(folder / "board.csv");
    std::ofstream rig(folder / "board-rig.csv");
    rig.exceptions(std::ios::badbit | std::ios::failbit); rig.imbue(std::locale::classic()); rig << std::setprecision(12);
    timeline.exceptions(std::ios::badbit | std::ios::failbit);
    poses.exceptions(std::ios::badbit | std::ios::failbit);
    board.exceptions(std::ios::badbit | std::ios::failbit);
    timeline.imbue(std::locale::classic()); poses.imbue(std::locale::classic()); board.imbue(std::locale::classic());
    timeline << std::setprecision(12);
    poses << std::setprecision(12);
    board << std::setprecision(12);
    timeline << "seconds,dt_s,generation,sequence,physics,trick,preset,custom_enabled,armed,goofy,live_switch,takeoff_goofy,takeoff_switch,recorded_goofy,recorded_switch,retarget,mirror,clip_time,weight,board_weight,failures,skater_publications,board_publications,skater_age_s,board_age_s,has_pose,observed_phase,entry,source_kind,episode,render_edit_sequence,render_presentation,board_edit_sequence,board_presentation,reverse_facing\n";
    const std::string columns = "seconds,sequence,stage,joint,sx,sy,sz,qx,qy,qz,qw,px,py,pz\n";
    poses << columns; board << columns;
    rig << columns;
    const double origin = history.samples.empty() ? 0 : history.samples.front().at;
    double previous = origin, max_gap = 0, max_position_delta = 0, max_angle = 0;
    std::size_t captured = 0, invalid = 0;
    const auto joint_row = [&](std::ostream& out, double at, std::uint64_t sequence, const char* stage, std::size_t index, const Joint& j) {
        out << at - origin << ',' << sequence << ',' << stage << ',' << index;
        for (auto value : j.scale) out << ',' << value;
        for (auto value : j.rotation) out << ',' << value;
        for (auto value : j.position) out << ',' << value;
        out << '\n';
    };
    const auto optional = [](std::ostream& out, auto value) { if (value) out << *value; out << ','; };
    for (const auto& s : history.samples) {
        const double gap = s.at - previous; previous = s.at;
        if (std::isfinite(gap)) max_gap = std::max(max_gap, gap);
        timeline << s.at - origin << ',' << gap << ',' << s.generation << ',' << s.sequence << ',';
        optional(timeline, s.physics);
        timeline << diagnostic_csv(s.trick) << ',' << diagnostic_csv(s.preset) << ',' << s.enabled << ',' << s.armed << ',' << s.goofy << ',';
        optional(timeline, s.live_switch); timeline << s.takeoff_goofy << ','; optional(timeline, s.takeoff_switch);
        timeline << s.recorded_goofy << ','; optional(timeline, s.recorded_switch);
        timeline << s.retarget << ',' << s.mirror << ',' << s.clip_time << ',' << s.weight << ',' << s.board_weight << ',' << s.failures << ','
                 << s.skater_publications << ',' << s.board_publications << ',' << s.skater_age << ',' << s.board_age << ',' << !s.native.empty() << ','
                 << diagnostic_csv(s.observed_phase) << ',' << diagnostic_csv(s.entry) << ',' << diagnostic_csv(s.source_kind) << ','
                 << s.episode << ',' << s.render_edit_sequence << ',' << s.render_presentation << ',' << s.board_edit_sequence << ',' << s.board_presentation << ',' << s.reverse_facing << '\n';
        if (!s.native.empty()) ++captured;
        for (std::size_t j = 0; j < s.native.size(); ++j) joint_row(poses, s.at, s.sequence, "animation_native", j, s.native[j]);
        for (std::size_t j = 0; j < s.desired.size(); ++j) {
            joint_row(poses, s.at, s.sequence, "desired", j, s.desired[j]);
            if (!finite(s.desired[j])) { ++invalid; continue; }
            if (j >= s.native.size() || !finite(s.native[j])) continue;
            double length = 0, dot = 0;
            for (int k = 0; k < 3; ++k) length += std::pow(s.desired[j].position[k] - s.native[j].position[k], 2);
            for (int k = 0; k < 4; ++k) dot += static_cast<double>(s.desired[j].rotation[k]) * s.native[j].rotation[k];
            max_position_delta = std::max(max_position_delta, std::sqrt(length));
            max_angle = std::max(max_angle, 2 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0)) * 180 / 3.141592653589793);
        }
        for (std::size_t j = 0; j < s.render_native.size(); ++j) joint_row(poses, s.render_at, s.sequence, "render_before_override", j, s.render_native[j]);
        for (std::size_t j = 0; j < s.render_edited.size(); ++j) joint_row(poses, s.render_edited_at, s.sequence, "render_after_edits", j, s.render_edited[j]);
        for (std::size_t j = 0; j < s.source.size(); ++j) joint_row(poses, s.at, s.sequence, "source_sample", j, s.source[j]);
        for (std::size_t j = 0; j < s.board_rig.size(); ++j) joint_row(rig, s.board_rig_at, s.sequence, "render_before_override", j, s.board_rig[j]);
        if (s.source_board) joint_row(board, s.at, s.sequence, "source_sample", board_joint, *s.source_board);
        if (s.native_board) joint_row(board, s.at, s.sequence, "animation_native", board_joint, *s.native_board);
        if (s.desired_board) joint_row(board, s.at, s.sequence, "desired", board_joint, *s.desired_board);
        if (s.render_board) joint_row(board, s.render_board_at, s.sequence, "render_before_override", board_joint, *s.render_board);
        if (s.render_edited_board) joint_row(board, s.render_edited_board_at, s.sequence, "render_after_edits", board_joint, *s.render_edited_board);
    }
    timeline.flush(); poses.flush(); board.flush(); rig.flush();
    Json summary = Json::object();
    summary["schema"] = 3;
    summary["samples"] = history.samples.size();
    summary["pose_samples"] = captured;
    summary["time_origin_qpc_seconds"] = origin;
    summary["max_animation_callback_gap_seconds"] = max_gap;
    summary["max_local_position_delta"] = max_position_delta;
    summary["max_local_rotation_delta_degrees"] = max_angle;
    summary["nonfinite_or_invalid_desired_joints"] = invalid;
    summary["scope"] = "Up to 15 seconds per independent gameplay/preview history; bounded at 4096 callbacks; full poses up to 10 Hz. Animation/body render/board render have separate timestamps. No graph branch or GPU final-pose claim.";
    summary["pose_space"] = "parent-local transforms";
    summary["board_space"] = "world-space board mount";
    summary["board_rig_space"] = "parent-local; 0 Reference, 1 placement child of 0, 2 mount child of 1; other hierarchy requires native rig mapping";
    summary["source_space"] = "loaded clip parent-local body and source-placement world board, before preview relocation and edits; may include loader sanitization";
    return summary;
}
} // namespace dingosdk::anim_studio
