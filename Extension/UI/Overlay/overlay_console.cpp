#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Log/logging.h"
#include "overlay_internal.h"
#include "Extension/Console/commands.h"
#include "console_suggestions.h"

namespace dingosdk::overlay::detail {

void append_console_line(std::string text,
    dingosdk::ConsoleSeverity severity = dingosdk::ConsoleSeverity::info,
    dingosdk::ConsoleSource source = dingosdk::ConsoleSource::command,
    std::uint64_t elapsed_ms = UINT64_MAX,
    dingosdk::logging::Context context = dingosdk::logging::Context::sdk) {
    auto& s = state();
    if (text.size() > maximum_console_line_length) {
        text.resize(maximum_console_line_length - 3);
        text += "...";
    }
    if (elapsed_ms == UINT64_MAX) elapsed_ms = dingosdk::console_timestamp_ms();
    std::size_t begin = 0;
    do {
        const auto end = text.find('\n', begin);
        auto part = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (!part.empty() && part.back() == '\r') part.pop_back();
        dingosdk::ConsoleLogLine entry{0, std::move(part), elapsed_ms, severity, source, context};
        if (s.console_lines.size() >= maximum_console_lines) s.console_lines.pop_front();
        s.console_lines.push_back({entry, dingosdk::format_console_line(entry),
            dingosdk::format_console_line(entry, false)});
        if (end == std::string::npos) break;
        begin = end + 1;
    } while (begin < text.size());
    s.console_scroll_to_bottom = true;
}

std::vector<ConsoleToken> console_tokens(std::string_view input, int cursor) {
    return dingosdk::console::completion_tokens(input, cursor);
}

std::vector<std::string> console_completion_candidates(const std::vector<ConsoleToken>& tokens) {
    auto& s = state();
    std::vector<std::string> words, result;
    for (const auto& token : tokens) words.push_back(token.text);
    s.console_suggestion_rows = dingosdk::console::game_commands().complete(words, s.model);
    // Plugins' commands beside ReSkate's own, word by word as ReSkate's: after "prop " come the
    // subcommands the plugin declared for "prop".
    if (!words.empty()) {
        const auto same = [](std::string_view a, std::string_view b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        };
        const auto& prefix = words.back();
        for (const auto& command : s.model.plugin_commands) {
            const auto path = dingosdk::parse_console_command(command.name).arguments;
            if (path.size() < words.size()) continue;
            bool matches = true;
            for (std::size_t i = 0; i + 1 < words.size() && matches; ++i) matches = same(words[i], path[i]);
            const auto& next = path[words.size() - 1];
            if (!matches || next.size() < prefix.size() || !same(std::string_view(next).substr(0, prefix.size()), prefix)) continue;
            if (std::any_of(s.console_suggestion_rows.begin(), s.console_suggestion_rows.end(), [&](const auto& row) { return same(row.text, next); }))
                continue;
            const bool last = path.size() == words.size();
            dingosdk::console::Suggestion row;
            row.text = row.name = next;
            if (last) row.name = command.name;
            row.description = (last ? command.help : std::string("Plugin command")) + " (" + command.plugin + ")";
            row.usage = last ? command.name + (command.args.empty() ? "" : " " + command.args) : next + " ...";
            s.console_suggestion_rows.push_back(std::move(row));
        }
    }
    for (const auto& row : s.console_suggestion_rows) result.push_back(row.text);
    return result;
}

std::string completion_text(const std::string& candidate, bool keep_quotes) {
    const bool quote = keep_quotes || std::any_of(candidate.begin(), candidate.end(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0 || character == '\\' || character == '"' || character == '\'';
    });
    if (!quote) return candidate;
    std::string output{"\""};
    for (char character : candidate) {
        if (character == '\\' || character == '"') output.push_back('\\');
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

void replace_console_input(ImGuiInputTextCallbackData* data, int begin, int end,
                           const std::string& replacement) {
    data->DeleteChars(begin, std::max(0, end - begin));
    data->InsertChars(begin, replacement.c_str());
}

std::string common_console_prefix(const std::vector<std::string>& candidates) {
    if (candidates.empty()) return {};
    std::size_t length = candidates.front().size();
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        length = std::min(length, candidates[i].size());
        std::size_t shared = 0;
        while (shared < length) {
            const auto left = static_cast<unsigned char>(candidates.front()[shared]);
            const auto right = static_cast<unsigned char>(candidates[i][shared]);
            if (std::tolower(left) != std::tolower(right)) break;
            ++shared;
        }
        length = shared;
    }
    return candidates.front().substr(0, length);
}

void reset_completion() {
    auto& s = state();
    s.completion_candidates.clear();
    s.completion_index = 0;
    s.completion_last_buffer.clear();
}

int console_input_callback(ImGuiInputTextCallbackData* data) {
    auto& s = state();
    s.console_cursor = data->CursorPos;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (!s.console_clicked_completion.empty()) {
            // Refocusing InputText can move its caret. Complete the token that
            // supplied the clicked row, preserving any arguments after it.
            const auto tokens = console_tokens(data->Buf, s.console_clicked_cursor);
            const auto& token = tokens.back();
            const auto replacement = completion_text(s.console_clicked_completion, token.quoted);
            replace_console_input(data, token.begin, token.end, replacement);
            int end = token.begin + static_cast<int>(replacement.size());
            if (end == data->BufTextLen || !std::isspace(static_cast<unsigned char>(data->Buf[end])))
                data->InsertChars(end, " ");
            data->CursorPos = std::min(end + 1, data->BufTextLen);
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
            s.console_clicked_completion.clear();
            reset_completion();
            s.console_cursor = data->CursorPos;
        }
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        reset_completion();
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        reset_completion();
        if (s.console_history.empty()) return 0;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (s.console_history_position < 0) {
                s.console_history_draft.assign(data->Buf, static_cast<std::size_t>(data->BufTextLen));
                s.console_history_position = static_cast<int>(s.console_history.size()) - 1;
            } else if (s.console_history_position > 0) --s.console_history_position;
        } else if (data->EventKey == ImGuiKey_DownArrow && s.console_history_position >= 0) {
            ++s.console_history_position;
            if (s.console_history_position >= static_cast<int>(s.console_history.size()))
                s.console_history_position = -1;
        }
        const std::string replacement = s.console_history_position < 0
            ? s.console_history_draft
            : s.console_history[static_cast<std::size_t>(s.console_history_position)];
        replace_console_input(data, 0, data->BufTextLen, replacement);
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackCompletion) return 0;

    const std::string current(data->Buf, static_cast<std::size_t>(data->BufTextLen));
    if (!s.completion_candidates.empty() && current == s.completion_last_buffer) {
        const auto& candidate = s.completion_candidates[s.completion_index % s.completion_candidates.size()];
        const auto replacement = completion_text(candidate, false);
        replace_console_input(data, s.completion_begin, s.completion_end, replacement);
        s.completion_end = s.completion_begin + static_cast<int>(replacement.size());
        s.completion_index = (s.completion_index + 1) % s.completion_candidates.size();
        s.completion_last_buffer.assign(data->Buf, static_cast<std::size_t>(data->BufTextLen));
        return 0;
    }

    reset_completion();
    const auto tokens = console_tokens(current, data->CursorPos);
    if (tokens.empty()) return 0;
    auto candidates = console_completion_candidates(tokens);
    if (candidates.empty()) return 0;
    const auto& token = tokens.back();
    if (candidates.size() == 1) {
        const auto replacement = completion_text(candidates.front(), token.quoted);
        replace_console_input(data, token.begin, token.end, replacement);
        const int end = token.begin + static_cast<int>(replacement.size());
        if (end == data->BufTextLen ||
            !std::isspace(static_cast<unsigned char>(data->Buf[end]))) data->InsertChars(end, " ");
        return 0;
    }

    const auto shared = common_console_prefix(candidates);
    const auto replacement = completion_text(shared, token.quoted);
    replace_console_input(data, token.begin, token.end, replacement);
    s.completion_candidates = std::move(candidates);
    s.completion_begin = token.begin;
    s.completion_end = token.begin + static_cast<int>(replacement.size());
    s.completion_last_buffer.assign(data->Buf, static_cast<std::size_t>(data->BufTextLen));
    return 0;
}

std::string trimmed_console_command(const char* input) {
    std::string command = input ? input : "";
    const auto first = std::find_if_not(command.begin(), command.end(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    });
    const auto last = std::find_if_not(command.rbegin(), command.rend(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

void show_console_history() {
    const auto& history = state().console_history;
    if (history.empty()) {
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::command, "History is empty.");
        return;
    }
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::command, "History:");
    for (std::size_t i = 0; i < history.size(); ++i)
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::command, "  " + std::to_string(i + 1) + "  " + history[i]);
}

void submit_console_command() {
    auto& s = state();
    const auto command = trimmed_console_command(s.console_input.data());
    s.console_input.fill('\0');
    s.console_cursor = 0;
    s.console_clicked_completion.clear();
    s.console_suggestions.clear();
    s.console_history_position = -1;
    s.console_history_draft.clear();
    reset_completion();
    if (command.empty()) return;
    if (s.console_history.empty() || s.console_history.back() != command) {
        if (s.console_history.size() >= 128) s.console_history.pop_front();
        s.console_history.push_back(command);
    }
    const auto parsed = dingosdk::parse_console_command(command);
    const dingosdk::console::Output output{
        [](const std::string& line) {
            dingosdk::logging::write(line.starts_with("error:") ? dingosdk::logging::Level::error : dingosdk::logging::Level::info, dingosdk::logging::Channel::command, line);
        },
        [&s] { s.console_log_sequence = dingosdk::logging::latest_sequence(); s.console_lines.clear(); s.console_scroll_to_bottom = false; s.console_force_scroll = false; },
        [] { show_console_history(); }
    };
    try {
        const auto& registry = dingosdk::console::game_commands();
        if (registry.echoes(parsed.arguments)) output("> " + command);
        // A command ReSkate does not know may be a plugin's, which run on the game thread
        // (Extension/Plugins); one no plugin has either is answered there as unknown.
        if (!parsed || (registry.knows(parsed.arguments) && registry.local(parsed.arguments, s.model))) {
            registry.execute(command, s.model, output);
            return;
        }
    } catch (const std::exception& error) {
        output(std::string("error: ") + error.what()); return;
    }
    if (!s.callbacks.queue_console_command) {
        dingosdk::logging::write(dingosdk::logging::Level::error, dingosdk::logging::Channel::command, "Command console is not connected.");
        return;
    }
    std::array<char, 1024> result{};
    const bool queued = s.callbacks.queue_console_command(
        s.callbacks.user, command.c_str(), result.data(), result.size());
    result.back() = '\0';
    if (!queued) dingosdk::logging::write(dingosdk::logging::Level::error, dingosdk::logging::Channel::command, result[0] ? result.data() : "Command rejected.");
}

void ingest_console_log(std::vector<dingosdk::overlay::ConsoleLogLine> lines) {
    auto& s = state();
    const auto by_sequence = [](const auto& left, const auto& right) {
        return left.sequence < right.sequence;
    };
    if (!std::is_sorted(lines.begin(), lines.end(), by_sequence))
        std::sort(lines.begin(), lines.end(), by_sequence);
    for (auto& line : lines) {
        if (!line.sequence || line.sequence <= s.console_log_sequence) continue;
        if (line.sequence - s.console_log_sequence > 1) {
            append_console_line("[... " + std::to_string(
                line.sequence - s.console_log_sequence - 1) + " log lines omitted ...]");
        }
        s.console_log_sequence = line.sequence;
        append_console_line(std::move(line.text), line.severity, line.source, line.elapsed_ms, line.context);
    }
}

bool console_row_matches(const ConsoleRow& row) {
    const auto& s = state();
    const auto source = static_cast<std::size_t>(row.entry.source);
    if (source < s.console_sources.size() && !s.console_sources[source]) return false;
    const auto context = static_cast<std::size_t>(row.entry.context);
    if (context < s.console_contexts.size() && !s.console_contexts[context]) return false;
    if (row.entry.severity == dingosdk::ConsoleSeverity::warning) {
        if (!s.console_show_warnings) return false;
    } else if (row.entry.severity == dingosdk::ConsoleSeverity::error || row.entry.severity == dingosdk::ConsoleSeverity::critical) {
        if (!s.console_show_errors) return false;
    } else if (!s.console_show_info) return false;
    return s.console_filter.PassFilter(row.formatted.c_str());
}

ImVec4 console_color(dingosdk::ConsoleSeverity severity, dingosdk::logging::Context context) {
    using Severity = dingosdk::ConsoleSeverity;
    using Context = dingosdk::logging::Context;
    if (severity == Severity::warning) return ImGui::ColorConvertU32ToFloat4(dingosdk::skate_theme::warning);
    if (severity == Severity::error || severity == Severity::critical)
        return ImGui::ColorConvertU32ToFloat4(dingosdk::skate_theme::danger);
    if (severity == Severity::success) return ImGui::ColorConvertU32ToFloat4(dingosdk::skate_theme::good);
    switch (context) {
    case Context::client: return {0.55f, 0.82f, 0.64f, 1.0f};
    case Context::server: return {0.40f, 0.62f, 0.95f, 1.0f};
    case Context::ui: return {0.75f, 0.62f, 0.88f, 1.0f};
    case Context::material: return {0.86f, 0.47f, 0.79f, 1.0f};
    case Context::filesystem: return {0.42f, 0.77f, 0.91f, 1.0f};
    case Context::audio: return {0.94f, 0.65f, 0.36f, 1.0f};
    case Context::engine: return {0.86f, 0.89f, 0.91f, 1.0f};
    default: return {0.62f, 0.67f, 0.71f, 1.0f};
    }
}

void copy_console_to_clipboard() {
    std::string output;
    for (const auto& line : state().console_lines) {
        if (!console_row_matches(line)) continue;
        output += state().console_timestamps ? line.formatted : line.plain;
        output.push_back('\n');
    }
    ImGui::SetClipboardText(output.c_str());
}

// A toggle tile for the console's severity filters: blue with black text when
// on, like the menu's selected tab, with the matching line count.
bool console_chip(ImDrawList* draw, const char* label, std::size_t count, bool& on, float k, unsigned seed) {
    auto& s = state();
    const std::string text = std::string(label) + "  " + std::to_string(count);
    const auto extent = s.menu.bold->CalcTextSizeA(14 * k, FLT_MAX, 0, text.c_str());
    const ImVec2 size(extent.x + 28 * k, ImGui::GetFrameHeight());
    const auto at = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##chip", size);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (pressed) on = !on;
    using namespace dingosdk::overlay::theme;
    dingosdk::skate_theme::rough_rect(draw, at, ImVec2(at.x + size.x, at.y + size.y),
        on ? blue : hovered ? dingosdk::skate_theme::tile_light : dingosdk::skate_theme::tile_grey, seed, k);
    draw->AddText(s.menu.bold, 14 * k, ImVec2(at.x + 14 * k, at.y + (size.y - extent.y) * .5f),
        on ? dingosdk::skate_theme::black : muted, text.c_str());
    return pressed;
}

// The OPTIONS panel: a styled drop-down under the button, laid out like the
// menu's pages instead of ImGui's default menu.
void draw_console_options(ImVec2 corner, float k) {
    auto& s = state();
    using namespace dingosdk::overlay::theme;
    namespace skate = dingosdk::skate_theme;
    const auto P = [k](float value) { return value * k; };
    ImGui::SetNextWindowPos(ImVec2(corner.x, corner.y + P(6)), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(P(480), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(P(18), P(16)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(P(8), P(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(9), P(6)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 18, 19, 252));
    if (ImGui::BeginPopup("Console options")) {
        auto* draw = ImGui::GetWindowDrawList();
        const auto origin = ImGui::GetWindowPos();
        draw->AddRectFilled(origin, ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y + P(3)), blue);
        const auto caption = [&](const char* text) {
            ImGui::PushFont(s.menu.bold);
            ImGui::TextDisabled("%s", text);
            ImGui::PopFont();
        };
        // A caption with small All / None buttons on the right.
        const auto group_caption = [&](const char* text, auto&& set_all) {
            caption(text);
            const float button = P(52);
            ImGui::SameLine(ImGui::GetContentRegionMax().x - button * 2 - ImGui::GetStyle().ItemSpacing.x);
            ImGui::PushID(text);
            if (ImGui::Button("All", ImVec2(button, 0))) set_all(true);
            ImGui::SameLine();
            if (ImGui::Button("None", ImVec2(button, 0))) set_all(false);
            ImGui::PopID();
        };
        const auto grid = [&](const char* id, std::size_t count, int columns, auto&& label, auto&& value) {
            if (!ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchSame)) return;
            for (std::size_t i = 0; i < count; ++i) {
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                ImGui::Checkbox(label(i), &value(i));
                ImGui::PopID();
            }
            ImGui::EndTable();
        };

        caption("ACTIONS");
        const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        if (ImGui::Button("Clear", ImVec2(third, 0))) {
            s.console_lines.clear();
            s.console_scroll_to_bottom = false;
            s.console_force_scroll = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy lines", ImVec2(third, 0))) copy_console_to_clipboard();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the lines the filters show.");
        ImGui::SameLine();
        if (ImGui::Button("Jump to latest", ImVec2(third, 0))) {
            s.console_scroll_to_bottom = true;
            s.console_force_scroll = true;
        }
        ImGui::Spacing();

        caption("VIEW");
        ImGui::Checkbox("Follow new messages", &s.console_autoscroll);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * .5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::Checkbox("Show timestamps", &s.console_timestamps);
        ImGui::Spacing();

        static constexpr std::array<const char*, dingosdk::logging::context_count> source_names{
            "SDK", "Client", "Server", "UI", "Engine", "Files", "Materials", "Audio"};
        group_caption("SOURCES", [&](bool on) { for (auto& value : s.console_contexts) value = on; });
        grid("##sources", s.console_contexts.size(), 4,
            [&](std::size_t i) { return source_names[i]; },
            [&](std::size_t i) -> bool& { return s.console_contexts[i]; });
        ImGui::Spacing();

        group_caption("CATEGORIES", [&](bool on) { for (auto& value : s.console_sources) value = on; });
        grid("##categories", s.console_sources.size(), 3,
            [&](std::size_t i) { return dingosdk::console_source_name(static_cast<dingosdk::ConsoleSource>(i)); },
            [&](std::size_t i) -> bool& { return s.console_sources[i]; });
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

void draw_console() {
    auto& s = state();
    if (!s.console_visible.load()) {
        s.console_input_active = false;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s.console_visible.store(false);
        s.console_input_active = false;
        return;
    }

    using namespace dingosdk::overlay::theme;
    namespace skate = dingosdk::skate_theme;
    // Same scale as the ReSkate menu (Settings > Interface > menu size).
    const float k = std::clamp(s.model.menu_scale, dingosdk::min_menu_scale, dingosdk::max_menu_scale);
    const auto P = [k](float value) { return value * k; };
    auto& io = ImGui::GetIO();
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = k;

    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 maximum(std::max(1.0f, viewport->WorkSize.x), std::max(1.0f, viewport->WorkSize.y));
    const ImVec2 initial_size(std::min(P(1120), maximum.x - 32), std::min(P(700), maximum.y - 32));
    ImGui::SetNextWindowPos(s.console_layout_valid ? s.console_position :
        ImVec2(viewport->WorkPos.x + 16, viewport->WorkPos.y + 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(s.console_layout_valid ? s.console_size : initial_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(P(560), maximum.x), std::min(P(420), maximum.y)), maximum);
    ImGui::PushFont(s.menu.body);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(9), P(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(P(8), P(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    const int colours = skate::push_widget_colours();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool opened = true;
    if (ImGui::Begin("Console###ReSkateConsole", nullptr, flags)) {
        const auto size = ImGui::GetWindowSize();
        const auto current = ImGui::GetWindowPos();
        const ImVec2 bounded(std::clamp(current.x, viewport->WorkPos.x,
            viewport->WorkPos.x + std::max(0.0f, maximum.x - size.x)),
            std::clamp(current.y, viewport->WorkPos.y, viewport->WorkPos.y + std::max(0.0f, maximum.y - size.y)));
        if (bounded.x != current.x || bounded.y != current.y) ImGui::SetWindowPos(bounded);
        const auto origin = ImGui::GetWindowPos();
        s.console_layout_valid = true;
        s.console_position = origin;
        s.console_size = size;
        auto* draw = ImGui::GetWindowDrawList();

        // Header band: brushed, tilted title like the menu's page headers.
        const float header = P(72);
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + header), IM_COL32(10, 10, 11, 250));
        {
            const int start = draw->VtxBuffer.Size;
            const ImVec2 at(origin.x + P(20), origin.y + P(12));
            draw->AddText(s.menu.title, P(40), ImVec2(at.x + P(2), at.y + P(3)), IM_COL32(0, 0, 0, 160), "CONSOLE");
            draw->AddText(s.menu.title, P(40), at, paper, "CONSOLE");
            const auto extent = s.menu.title->CalcTextSizeA(P(40), FLT_MAX, 0, "CONSOLE");
            skate::rotate_since(draw, start, -3.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
            draw->AddText(s.menu.body, P(14), ImVec2(at.x + extent.x + P(18), at.y + P(20)), muted,
                "Commands, logs and live values.");
        }
        halftone(draw, ImVec2(origin.x + size.x - P(150), origin.y + P(18)), 10, 6, IM_COL32(61, 63, 64, 140));
        ImGui::SetCursorPos(ImVec2(size.x - P(44), P(16)));
        if (ImGui::Button("X", ImVec2(P(28), P(28)))) opened = false;

        ImGui::SetCursorPos(ImVec2(P(16), header + P(12)));
        ImGui::BeginChild("##console-body", ImVec2(size.x - P(32), size.y - header - P(24)), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto* body = ImGui::GetWindowDrawList();

        // Filter row: severity tiles, the search box and the options menu.
        std::size_t infos = 0, warnings = 0, errors = 0;
        for (const auto& line : s.console_lines) {
            using Severity = dingosdk::ConsoleSeverity;
            if (line.entry.severity == Severity::warning) ++warnings;
            else if (line.entry.severity == Severity::error || line.entry.severity == Severity::critical) ++errors;
            else ++infos;
        }
        console_chip(body, "MESSAGES", infos, s.console_show_info, k, 21);
        ImGui::SameLine(0, P(6));
        console_chip(body, "WARNINGS", warnings, s.console_show_warnings, k, 22);
        ImGui::SameLine(0, P(6));
        console_chip(body, "ERRORS", errors, s.console_show_errors, k, 23);
        ImGui::SameLine(0, P(12));
        const float options_width = s.menu.bold->CalcTextSizeA(P(15), FLT_MAX, 0, "OPTIONS").x + P(24);
        s.console_filter.Draw("##Console filter", ImGui::GetContentRegionAvail().x - options_width - P(8));
        if (!s.console_filter.IsActive() && !ImGui::IsItemActive()) {
            const auto at = ImGui::GetItemRectMin();
            body->AddText(s.menu.body, P(16), ImVec2(at.x + P(9), at.y + P(6)), muted, "Filter messages...");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search messages or sources. Use -text to exclude matches.");
        ImGui::SameLine(0, P(8));
        ImGui::PushFont(s.menu.bold);
        if (ImGui::Button("OPTIONS", ImVec2(options_width, 0))) ImGui::OpenPopup("Console options");
        ImGui::PopFont();
        const auto options_corner = ImGui::GetItemRectMax();
        draw_console_options(options_corner, k);

        ImGui::PushFont(s.menu.mono);
        s.console_suggestions = s.console_input[0]
            ? console_completion_candidates(console_tokens(s.console_input.data(), s.console_cursor))
            : std::vector<std::string>{};
        // Keep rows visible while they take focus on mouse-down; Selectable
        // accepts the click on mouse-up in a later frame.
        const bool suggestions_open = !s.console_suggestions.empty();
        const float input_height = ImGui::GetFrameHeightWithSpacing();
        const float footer_height = P(28);
        const float divider_height = P(6);
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float divider_space = suggestions_open ? divider_height + spacing : 0;
        const float panels_height = std::max(1.0f, ImGui::GetContentRegionAvail().y - input_height -
            footer_height - divider_space - 2 * spacing);
        const ImVec2 panel_padding(P(12), P(10));
        const float content_height = ImGui::GetTextLineHeightWithSpacing() *
            static_cast<float>(s.console_suggestions.size()) + 2 * panel_padding.y + ImGui::GetStyle().ScrollbarSize;
        const float suggestions_height = suggestions_open ? std::min(content_height,
            std::clamp(panels_height * s.console_suggestions_fraction,
                std::min(P(80), panels_height), std::max(P(80), panels_height - P(48)))) : 0;
        const float scrollback_height = std::max(1.0f, panels_height - suggestions_height);
        std::vector<std::size_t> filtered;
        for (std::size_t i = 0; i < s.console_lines.size(); ++i)
            if (console_row_matches(s.console_lines[i])) filtered.push_back(i);

        // Scrollback on a rough-cut tile, like the menu's panels.
        {
            const auto at = ImGui::GetCursorScreenPos();
            skate::rough_rect(body, at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + scrollback_height), skate::tile, 31, k);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(P(4), P(3)));
        if (ImGui::BeginChild("Console scrollback", ImVec2(0, scrollback_height),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(filtered.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto& line = s.console_lines[filtered[static_cast<std::size_t>(row)]];
                    ImGui::PushStyleColor(ImGuiCol_Text, console_color(line.entry.severity, line.entry.context));
                    ImGui::TextUnformatted((s.console_timestamps ? line.formatted : line.plain).c_str());
                    ImGui::PopStyleColor();
                }
            }
            if (filtered.empty())
                ImGui::TextDisabled(s.console_lines.empty() ? "Nothing logged yet." : "No lines match the filters.");
            if (s.console_scroll_to_bottom && (s.console_force_scroll ||
                (s.console_autoscroll && s.console_was_at_bottom)))
                ImGui::SetScrollHereY(1.0f);
            s.console_scroll_to_bottom = false;
            s.console_force_scroll = false;
            s.console_was_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);

        if (suggestions_open) {
            const auto at = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("Resize command list", ImVec2(-1, divider_height));
            const bool dragging = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();
            if (hovered || dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (dragging && panels_height > 0) {
                s.console_suggestions_fraction = std::clamp(s.console_suggestions_fraction -
                    ImGui::GetIO().MouseDelta.y / panels_height, 0.2f, 0.9f);
            }
            if (hovered && !dragging) ImGui::SetTooltip("Drag to resize the command list");
            const float middle = at.y + divider_height * .5f;
            body->AddRectFilled(ImVec2(at.x, middle - P(1)), ImVec2(at.x + ImGui::GetItemRectSize().x, middle + P(1)),
                hovered || dragging ? blue : skate::tile_light);
            if (s.console_resizing_suggestions && !dragging) s.console_focus_requested.store(true);
            s.console_resizing_suggestions = dragging;
        }

        // Command line: blue prompt mark, the input, and a primary RUN button.
        if (s.console_focus_requested.exchange(false)) ImGui::SetKeyboardFocusHere();
        constexpr ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackEdit;
        const float submit_width = P(84);
        const float command_height = ImGui::GetFrameHeight();
        {
            const auto at = ImGui::GetCursorScreenPos();
            body->AddRectFilled(at, ImVec2(at.x + P(4), at.y + command_height), blue);
            ImGui::SetCursorScreenPos(ImVec2(at.x + P(4), at.y));
        }
        ImGui::SetNextItemWidth(-submit_width - ImGui::GetStyle().ItemSpacing.x);
        bool submitted = ImGui::InputTextWithHint("##Console input", "Enter a command...",
            s.console_input.data(), s.console_input.size(), input_flags,
            console_input_callback, nullptr);
        s.console_input_active = ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::PushFont(s.menu.bold);
        skate::push_primary_button();
        submitted |= ImGui::Button("RUN", ImVec2(submit_width, command_height));
        skate::pop_primary_button();
        ImGui::PopFont();
        if (submitted) {
            submit_console_command();
            s.console_focus_requested.store(true);
        }
        if (suggestions_open && suggestions_height > 0 && !submitted) {
            const auto at = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            skate::rough_rect(body, at, ImVec2(at.x + width, at.y + suggestions_height), skate::tile, 32, k);
            body->AddRectFilled(at, ImVec2(at.x + P(4), at.y + suggestions_height), blue);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
            if (ImGui::BeginChild("Console suggestions", ImVec2(0, suggestions_height),
                ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const auto& row : s.console_suggestion_rows) {
                    if (draw_console_suggestion(row, paper, muted)) {
                        s.console_clicked_completion = row.text;
                        s.console_clicked_cursor = s.console_cursor;
                        s.console_focus_requested.store(true);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        ImGui::PopFont();

        // Footer: keycap hints like the menu's, and the line count.
        {
            const auto at = ImGui::GetCursorScreenPos();
            const float y = at.y + std::max(0.0f, ImGui::GetContentRegionAvail().y - P(22));
            static const auto close_key = dingosdk::launcher::key_cap(dingosdk::launcher::overlay_keys().console);
            float x = at.x;
            const float width = ImGui::GetContentRegionAvail().x;
            if (width >= P(620)) {
                x += keycap(body, s.menu.bold, k, ImVec2(x, y), "TAB", "Complete") + P(18);
                x += keycap(body, s.menu.bold, k, ImVec2(x, y), "\xE2\x86\x91\xE2\x86\x93", "History") + P(18);
            }
            keycap(body, s.menu.bold, k, ImVec2(x, y), close_key.c_str(), "or");
            x += s.menu.bold->CalcTextSizeA(P(12), FLT_MAX, 0, close_key.c_str()).x +
                 s.menu.bold->CalcTextSizeA(P(14), FLT_MAX, 0, "or").x + P(28);
            keycap(body, s.menu.bold, k, ImVec2(x, y), "ESC", "Close");
            const auto count = std::to_string(filtered.size()) + " / " + std::to_string(s.console_lines.size()) + " lines";
            const auto extent = s.menu.body->CalcTextSizeA(P(14), FLT_MAX, 0, count.c_str());
            body->AddText(s.menu.body, P(14), ImVec2(at.x + width - extent.x, y + P(2)), muted, count.c_str());
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(9);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
    if (!opened) { s.console_visible.store(false); s.console_input_active = false; }
}
}
