#include "overlay_internal.h"
#include "skate_style.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Core/Platform/path_text.h"
#include <cstdio>
#include <format>

// The profiler's HUD and window (Engine/Core/Profiling/profiler.h; `perf` console commands).
// The HUD draws under everything and takes no input, so it can stay up while playing. The window
// is interactive and shows only while the menu or console already holds the pointer.

namespace dingosdk::overlay::detail {
namespace {
namespace skate = dingosdk::skate_theme;

float scale() {
    return std::clamp(state().model.menu_scale, dingosdk::min_menu_scale, dingosdk::max_menu_scale);
}

// Frame-time bars against a budget line: blue within it, amber up to twice it, red beyond.
void bars(ImDrawList* draw, ImVec2 at, ImVec2 size, std::span<const float> values, float budget) {
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(0, 0, 0, 110));
    if (values.empty()) return;
    float top = budget * 2.5f;
    for (const auto v : values) top = std::max(top, std::min(v, budget * 8));
    const float width = size.x / static_cast<float>(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float v = std::min(values[i], top);
        const float h = std::max(1.0f, size.y * v / top);
        const ImU32 colour = values[i] <= budget * 1.05f ? skate::blue : values[i] <= budget * 2 ? skate::warning : skate::danger;
        const float x = at.x + width * static_cast<float>(i);
        draw->AddRectFilled(ImVec2(x, at.y + size.y - h), ImVec2(x + std::max(1.0f, width - 0.5f), at.y + size.y), colour);
    }
    const float line = at.y + size.y - size.y * budget / top;
    draw->AddLine(ImVec2(at.x, line), ImVec2(at.x + size.x, line), IM_COL32(255, 255, 255, 120), 1.0f);
}

std::span<const float> recent(profiler::Series series, std::array<float, 240>& buffer) {
    return {buffer.data(), profiler::history(series, buffer)};
}

// What a frame of the client update is allowed: the rate it has been running at, rounded to the
// usual sim rates, so 60 Hz measures against 16.7 ms and 30 Hz against 33.3 ms.
float client_budget(const profiler::Summary& s) {
    const double rate = s.client_updates_per_second;
    return rate > 45 ? 1000.0f / 60 : rate > 0 ? 1000.0f / 30 : 1000.0f / 60;
}
} // namespace

bool perf_hud_pending() { return profiler::hud(); }

void draw_perf_hud() {
    if (!profiler::hud()) return;
    auto& s = state();
    const float k = scale();
    const auto P = [k](float value) { return value * k; };
    auto* font = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* bold = s.menu.bold ? s.menu.bold : font;
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto summary = profiler::summary();
    const auto& m = *summary;
    const float width = P(300), size = P(13.5f), line = P(17);
    const ImVec2 origin(P(12), P(12));
    ImVec2 at(origin.x + P(10), origin.y + P(8));
    const auto text = [&](ImFont* f, const std::string& value, ImU32 colour) {
        draw->AddText(f, size, at, colour, value.c_str());
        at.y += line;
    };
    // Background first, sized after the fact: draw into a channel behind the text.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    std::array<float, 240> buffer{};
    const float budget = client_budget(m);
    // The client frame job's time against the sim budget; without it (hook not installed), the
    // spacing of client updates.
    const bool framed = m.client_frame_average_ms > 0;
    const double frame = framed ? m.client_frame_average_ms : m.client_interval_average_ms;
    text(bold, std::format("CLIENT  {:.0f}/s   frame {:.1f} / {:.1f} ms", m.client_updates_per_second, frame, budget),
         frame <= budget * 0.85 ? skate::white : frame <= budget ? skate::warning : skate::danger);
    text(font, std::format("ReSkate {:.2f} ms tick + {:.2f} ms hooks per frame", m.client_own_average_ms,
         m.hooks_milliseconds_per_update), skate::grey_text);
    text(font, std::format("longest: frame {:.1f}  interval {:.1f}  ReSkate {:.1f} ms", m.client_frame_longest_ms,
         m.client_interval_longest_ms, m.client_own_longest_ms),
         m.client_frame_longest_ms > budget || m.client_own_longest_ms > budget * 0.25f ? skate::warning : skate::grey_text);
    bars(draw, ImVec2(at.x, at.y + P(2)), ImVec2(width - P(20), P(34)),
         recent(framed ? profiler::Series::client_frame : profiler::Series::client_interval, buffer), budget);
    at.y += P(40);
    text(bold, std::format("FRAMES  {:.0f}/s   {:.2f} ms", m.presents_per_second, m.present_interval_average_ms), skate::white);
    text(font, std::format("longest {:.1f} ms", m.present_interval_longest_ms), skate::grey_text);
    const float frame_budget = m.present_interval_average_ms > 0 ? static_cast<float>(m.present_interval_average_ms) * 1.5f : 16.7f;
    bars(draw, ImVec2(at.x, at.y + P(2)), ImVec2(width - P(20), P(24)), recent(profiler::Series::present_interval, buffer), frame_budget);
    at.y += P(30);
    double client_cpu{};
    for (const auto& thread : m.threads) if (thread.id == m.client_thread) client_cpu = thread.cpu_percent;
    text(bold, std::format("CPU  {:.0f}% of {} cores   client thread {:.0f}%", m.process_cpu_percent, m.cores, client_cpu), skate::white);
    // A ReSkate thread using most of a core is never expected: name it.
    for (const auto& thread : m.threads)
        if (thread.label == "ReSkate worker" && thread.cpu_percent >= 50)
            text(bold, std::format("ReSkate worker {} at {:.0f}%", thread.id, thread.cpu_percent), skate::danger);
    if (!m.zones.empty()) {
        const auto& top = m.zones.front();
        text(font, std::format("busiest: {} {:.2f} ms/s", top.name, top.milliseconds_per_second), skate::grey_text);
    }
    if (const auto report = profiler::sample_report(); report && report->running)
        text(bold, std::format("SAMPLING {}  {:.0f}%", report->target, report->progress * 100), skate::blue);
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, at.y + P(6)), IM_COL32(12, 12, 13, 196), P(4));
    draw->ChannelsMerge();
}

void draw_perf_window() {
    if (!profiler::window()) return;
    auto& s = state();
    const float k = scale();
    const auto P = [k](float value) { return value * k; };
    auto& io = ImGui::GetIO();
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = k;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - P(760) - 16, viewport->WorkPos.y + 16),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(P(760), std::min(P(720), viewport->WorkSize.y - 32)), ImGuiCond_FirstUseEver);
    ImGui::PushFont(s.menu.body);
    const int colours = skate::push_widget_colours();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    bool open = true;
    if (ImGui::Begin("ReSkate profiler###ReSkateProfiler", &open, ImGuiWindowFlags_NoCollapse)) {
        const auto summary = profiler::summary();
        const auto& m = *summary;
        std::array<float, 240> buffer{};
        if (ImGui::BeginTabBar("##perf_tabs")) {
            if (ImGui::BeginTabItem("Overview")) {
                const float budget = client_budget(m);
                ImGui::Text("Client update: %.1f/s, every %.2f ms (longest %.1f ms)", m.client_updates_per_second,
                    m.client_interval_average_ms, m.client_interval_longest_ms);
                ImGui::Text("  client frame %.2f ms (longest %.1f) of a %.1f ms budget", m.client_frame_average_ms,
                    m.client_frame_longest_ms, budget);
                ImGui::Text("  ReSkate tick %.2f ms (longest %.1f), ReSkate hooks %.2f ms per frame", m.client_own_average_ms,
                    m.client_own_longest_ms, m.hooks_milliseconds_per_update);
                ImGui::TextDisabled("The client frame includes ReSkate's tick and hooks. Over budget, the game runs in slow motion.");
                const auto graph = [&](const char* label, profiler::Series series, float height) {
                    const auto values = recent(series, buffer);
                    const auto at = ImGui::GetCursorScreenPos();
                    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
                    ImGui::Dummy(size);
                    bars(ImGui::GetWindowDrawList(), at, size, values,
                        series == profiler::Series::present_interval && m.present_interval_average_ms > 0 ?
                            static_cast<float>(m.present_interval_average_ms) * 1.5f : budget);
                    ImGui::TextDisabled("%s", label);
                };
                graph("Client update interval (ms)", profiler::Series::client_interval, P(70));
                graph("Client frame time (ms)", profiler::Series::client_frame, P(50));
                graph("ReSkate tick time (ms)", profiler::Series::client_own, P(40));
                ImGui::Separator();
                ImGui::Text("Frames: %.1f/s, %.2f ms (longest %.1f ms)", m.presents_per_second, m.present_interval_average_ms,
                    m.present_interval_longest_ms);
                graph("Frame interval (ms)", profiler::Series::present_interval, P(50));
                ImGui::Text("Process CPU: %.0f%% (%u cores = %u%%)", m.process_cpu_percent, m.cores, m.cores * 100);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("ReSkate")) {
                ImGui::TextDisabled("ReSkate's own work, timed. \"hooks/\" zones run inside the game's update.");
                if (ImGui::BeginTable("##zones", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                    for (const char* name : {"ms/s", "per update", "calls/s", "avg us", "longest ms"})
                        ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableHeadersRow();
                    for (const auto& zone : m.zones) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(zone.name.c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%.2f", zone.milliseconds_per_second);
                        ImGui::TableNextColumn();
                        if (m.client_updates_per_second > 0) ImGui::Text("%.3f ms", zone.milliseconds_per_second / m.client_updates_per_second);
                        ImGui::TableNextColumn(); ImGui::Text("%.0f", zone.calls_per_second);
                        ImGui::TableNextColumn(); ImGui::Text("%.1f", zone.average_microseconds);
                        ImGui::TableNextColumn();
                        if (zone.longest_milliseconds >= 2) ImGui::TextColored(ImColor(skate::warning), "%.2f", zone.longest_milliseconds);
                        else ImGui::Text("%.2f", zone.longest_milliseconds);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Threads")) {
                ImGui::TextDisabled("CPU per thread over the last second; 100%% is one whole core.");
                if (ImGui::BeginTable("##threads", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 2.5f);
                    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableHeadersRow();
                    for (const auto& thread : m.threads) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%u", thread.id);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(thread.label.c_str());
                        ImGui::TableNextColumn();
                        if (thread.cpu_percent >= 90) ImGui::TextColored(ImColor(skate::danger), "%.1f%%", thread.cpu_percent);
                        else ImGui::Text("%.1f%%", thread.cpu_percent);
                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(thread.id));
                        if (ImGui::SmallButton("Sample")) {
                            std::string error;
                            profiler::start_sampling({profiler::Target::thread, thread.id, 10, 1000}, error);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sampler")) {
                static int target = 0, rate_index = 2;
                static float seconds = 10;
                static char filter[96]{};
                static std::string error;
                constexpr unsigned rates[] = {250, 500, 1000, 2000};
                ImGui::RadioButton("Client update thread", &target, 0); ImGui::SameLine();
                ImGui::RadioButton("Present thread", &target, 1); ImGui::SameLine();
                ImGui::RadioButton("All busy threads", &target, 2);
                ImGui::SetNextItemWidth(P(220));
                ImGui::SliderFloat("seconds", &seconds, 1, 30, "%.0f s");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(P(120));
                ImGui::Combo("Hz", &rate_index, "250\0" "500\0" "1000\0" "2000\0");
                const auto report = profiler::sample_report();
                if (report && report->running) {
                    if (ImGui::Button("Stop")) profiler::stop_sampling();
                    ImGui::SameLine();
                    ImGui::ProgressBar(static_cast<float>(report->progress), ImVec2(-1, 0),
                        std::format("{} samples", report->samples).c_str());
                } else {
                    skate::push_primary_button();
                    if (ImGui::Button("Start sample")) {
                        error.clear();
                        const auto which = target == 2 ? profiler::Target::all : target == 1 ? profiler::Target::present
                                                                                              : profiler::Target::client;
                        // Every busy thread is sampled per tick: a quarter of the rate keeps the cost down.
                        profiler::start_sampling({which, 0, seconds, target == 2 ? std::max(50u, rates[rate_index] / 4)
                                                                                  : rates[rate_index]}, error);
                    }
                    skate::pop_primary_button();
                }
                if (!error.empty()) ImGui::TextColored(ImColor(skate::danger), "%s", error.c_str());
                if (report && !report->running && report->samples) {
                    ImGui::Separator();
                    ImGui::Text("%s (thread %u): %u samples over %.1f s; busy %.1f%%", report->target.c_str(), report->thread_id,
                        report->samples, report->seconds, report->busy_percent);
                    for (const auto& thread : report->threads)
                        ImGui::TextDisabled("  %5.1f%%  thread %u  %s", thread.cpu_percent, thread.id, thread.label.c_str());
                    if (!report->error.empty()) ImGui::TextColored(ImColor(skate::danger), "%s", report->error.c_str());
                    const auto report_path = path_utf8(report->report), folded_path = path_utf8(report->folded);
                    ImGui::TextDisabled("Report: %s", report_path.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy##report")) ImGui::SetClipboardText(report_path.c_str());
                    ImGui::TextDisabled("Flame graph (speedscope.app): %s", folded_path.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy##folded")) ImGui::SetClipboardText(folded_path.c_str());
                    ImGui::SetNextItemWidth(P(260));
                    ImGui::InputTextWithHint("##filter", "filter functions", filter, sizeof(filter));
                    ImGui::SameLine();
                    ImGui::TextDisabled("Click a row to copy its address (Skate ones are Ghidra addresses).");
                    if (ImGui::BeginTable("##functions", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 6.0f);
                        ImGui::TableHeadersRow();
                        const std::string_view wanted(filter);
                        for (const auto& f : report->functions) {
                            if (!wanted.empty() && f.name.find(wanted) == std::string::npos) continue;
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::Text("%.1f%%", 100.0 * f.self / report->samples);
                            ImGui::TableNextColumn(); ImGui::Text("%.1f%%", 100.0 * f.total / report->samples);
                            ImGui::TableNextColumn();
                            ImGui::PushID(&f);
                            if (ImGui::Selectable(f.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                                ImGui::SetClipboardText(std::format("0x{:x}", f.address).c_str());
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(colours);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
    if (!open) profiler::set_window(false);
}
} // namespace dingosdk::overlay::detail
