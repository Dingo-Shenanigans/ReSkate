#include "plugin_overlay.h"
#include "Extension/Plugins/plugin_camera.h"
#include "Extension/Plugins/plugin_ui.h"
#include "Extension/UI/Overlay/game_images.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Extension/UI/Overlay/input_capture.h"
#include "Extension/UI/Overlay/xray_overlay.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/skate_style.h"
#include <algorithm>
#include <imgui_internal.h>
#include <memory>
#include <chrono>
#include <mutex>
#include <string>

namespace dingosdk::overlay {
namespace {
// Render thread: the layers taken by plugins_pending, drawn by draw_plugins.
std::shared_ptr<const std::vector<plugin_ui::Layer>>& frame() {
    static auto* value = new std::shared_ptr<const std::vector<plugin_ui::Layer>>;
    return *value;
}
ImVec2 at(const plugin_ui::Vec3& p) { return ImVec2(p[0], p[1]); }
ImU32 shadow_of(ImU32 colour) {
    const auto alpha = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xff) * 0.75f);
    return IM_COL32(0, 0, 0, alpha);
}
void text(ImDrawList* draw, ImFont* font, float k, ImVec2 where, const plugin_ui::Shape& shape) {
    const float size = shape.style.size * k;
    if (shape.style.wrap > 0) {
        // Wrapped: from its top left, as d:measure counts it.
        const float wrap = shape.style.wrap * k;
        if (shape.style.shadow) {
            const float offset = std::max(1.0f, size * 0.06f);
            draw->AddText(font, size, ImVec2(where.x + offset, where.y + offset), shadow_of(shape.style.colour), shape.text.c_str(), nullptr, wrap);
        }
        draw->AddText(font, size, where, shape.style.colour, shape.text.c_str(), nullptr, wrap);
        return;
    }
    const auto width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, shape.text.c_str()).x;
    if (shape.style.align == 1) where.x -= width * 0.5f;
    else if (shape.style.align == 2) where.x -= width;
    if (shape.style.shadow) {
        const float offset = std::max(1.0f, size * 0.06f);
        draw->AddText(font, size, ImVec2(where.x + offset, where.y + offset), shadow_of(shape.style.colour), shape.text.c_str());
    }
    draw->AddText(font, size, where, shape.style.colour, shape.text.c_str());
}
void circle(ImDrawList* draw, ImVec2 centre, float radius, const plugin_ui::Style& style, float k) {
    if (radius <= 0) return;
    if (style.fill) draw->AddCircleFilled(centre, radius, style.colour);
    else draw->AddCircle(centre, radius, style.colour, 0, style.thickness * k);
}
}

// The mouse while a plugin's screen is open, for reskate.input.cursor (render thread writes it).
struct CursorState {
    std::mutex mutex;
    PluginCursor now;
    bool open{};
};
static CursorState& cursor_state() {
    static auto* value = new CursorState;
    return *value;
}
bool read_plugin_cursor(PluginCursor& out) noexcept {
    auto& c = cursor_state();
    std::lock_guard lock(c.mutex);
    if (!c.open) return false;
    out = c.now;
    c.now.wheel = 0;
    return true;
}
// Typing a plugin takes (take_plugin_text): until when, and what came.
struct Typing {
    std::mutex mutex;
    std::string text;
    std::chrono::steady_clock::time_point until{};
};
static Typing& typing() {
    static auto* value = new Typing;
    return *value;
}
static bool typing_taken() {
    auto& t = typing();
    std::lock_guard lock(t.mutex);
    return std::chrono::steady_clock::now() < t.until;
}
std::string take_plugin_text(bool capture) noexcept {
    try {
        auto& t = typing();
        std::lock_guard lock(t.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (capture) t.until = now + std::chrono::milliseconds(300);
        else t.until = {};
        return std::exchange(t.text, {});
    } catch (...) {
        return {};
    }
}
// Render thread, while a plugin's screen is open: the characters typed this frame, kept for it.
static void note_typing() {
    if (!typing_taken()) return;
    std::string typed;
    for (const ImWchar c : ImGui::GetIO().InputQueueCharacters) {
        const auto u = static_cast<unsigned>(c);
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0)) continue;
        if (u < 0x80) typed += static_cast<char>(u);
        else if (u < 0x800) {
            typed += static_cast<char>(0xc0 | (u >> 6));
            typed += static_cast<char>(0x80 | (u & 0x3f));
        } else {
            typed += static_cast<char>(0xe0 | (u >> 12));
            typed += static_cast<char>(0x80 | ((u >> 6) & 0x3f));
            typed += static_cast<char>(0x80 | (u & 0x3f));
        }
    }
    if (typed.empty()) return;
    auto& t = typing();
    std::lock_guard lock(t.mutex);
    if (t.text.size() + typed.size() <= 1024) t.text += typed;
}
static void note_cursor(bool open, bool over_screen) {
    auto& c = cursor_state();
    std::lock_guard lock(c.mutex);
    c.open = open;
    if (!open) {
        c.now = {};
        return;
    }
    const auto& io = ImGui::GetIO();
    c.now.x = io.MousePos.x;
    c.now.y = io.MousePos.y;
    c.now.left = io.MouseDown[0];
    c.now.right = io.MouseDown[1];
    // The middle button also as Windows and raw input have it: in game it did not reach ImGui's state
    // while the left and right ones did (a plugin's pan never started).
    bool middle = io.MouseDown[2];
    if (!middle) {
        auto& s = detail::state();
        const auto raw_at = s.raw_mouse_time.load();
        detail::OverlayInputAccess real;
        middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                 (raw_at && GetTickCount64() < raw_at + 10000 && (s.raw_mouse_buttons.load() & 4u) != 0);
    }
    c.now.middle = middle;
    c.now.wheel += io.MouseWheel;
    c.now.over_screen = over_screen;
}

// The screen Esc closed, until its plugin's next build no longer has it open.
static std::string& closed_screen() {
    static auto* value = new std::string;
    return *value;
}
static const plugin_ui::Panel* open_screen() {
    const auto panels = plugin_ui::latest_panels();
    static std::shared_ptr<const std::vector<plugin_ui::Panel>> held;
    held = panels;
    if (!held) return nullptr;
    for (const auto& panel : *held)
        if (panel.screen) {
            if (closed_screen() == panel.plugin + '\n' + panel.title) return nullptr;
            return &panel;
        }
    closed_screen().clear();
    return nullptr;
}

// Called before the overlay's ImGui context exists or is made current, too: its size is read from
// the context itself (what its last frame had), never through ImGui's current one.
bool plugins_pending() {
    detail::state().plugin_screen_visible.store(open_screen() != nullptr);
    if (const auto* context = detail::state().context) {
        const auto display = context->IO.DisplaySize;
        if (display.x > 0 && display.y > 0) plugin_ui::set_display(display.x, display.y);
    }
    frame() = plugin_ui::latest_layers();
    return frame() && !frame()->empty();
}

void draw_plugin_screen() {
    const auto* screen = open_screen();
    if (!screen) {
        note_cursor(false, false);
        return;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    const float k = std::max(1.0f, display.y / 1080.0f);
    auto& s = detail::state();
    // No panel: the input is the plugin's, which draws its screen itself; Esc still closes it.
    note_typing();
    if (screen->dock == 3) {
        note_cursor(true, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput && !typing_taken()) {
            plugin_ui::act({screen->plugin, screen->title, plugin_ui::close_widget});
            closed_screen() = screen->plugin + '\n' + screen->title;
            s.plugin_screen_visible.store(false);
        }
        return;
    }
    // Docked: a panel down one side, the game showing beside it.
    const bool docked = screen->dock != 0;
    const float panel = docked ? std::round(display.x * std::clamp(screen->width, 0.2f, 0.6f)) : display.x;
    ImGui::SetNextWindowPos(ImVec2(screen->dock == 2 ? display.x - panel : 0.0f, 0));
    ImGui::SetNextWindowSize(ImVec2(panel, display.y));
    ImGui::SetNextWindowBgAlpha(docked ? 0.88f : 0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, docked ? ImVec2(24 * k, 24 * k) : ImVec2(64 * k, 48 * k));
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    const auto& io = ImGui::GetIO();
    const float left = screen->dock == 2 ? display.x - panel : 0.0f;
    note_cursor(true, io.MousePos.x >= left && io.MousePos.x < left + panel);
    if (ImGui::Begin("##plugin-screen", nullptr, flags)) {
        const float wide = docked ? ImGui::GetContentRegionAvail().x * 0.6f : 480 * k;
        const float button = docked ? ImGui::GetContentRegionAvail().x : 320 * k;
        ImGui::SetWindowFontScale(1.25f * k);
        if (s.menu.heading) ImGui::PushFont(s.menu.heading);
        ImGui::TextUnformatted(screen->title.c_str());
        if (s.menu.heading) ImGui::PopFont();
        ImGui::TextDisabled("%s  -  Esc closes it", screen->plugin.c_str());
        ImGui::Separator();
        ImGui::Spacing();
        int item = 0;
        for (const auto& w : screen->widgets) {
            ImGui::PushID(item++);
            plugin_ui::Action action{screen->plugin, screen->title, w.id};
            bool acted = false;
            using Kind = plugin_ui::Widget::Kind;
            switch (w.kind) {
            case Kind::heading:
                ImGui::Spacing();
                if (s.menu.bold) ImGui::PushFont(s.menu.bold);
                ImGui::TextUnformatted(w.text.c_str());
                if (s.menu.bold) ImGui::PopFont();
                break;
            case Kind::label: ImGui::TextWrapped("%s", w.text.c_str()); break;
            case Kind::separator: ImGui::Separator(); break;
            case Kind::card:
                ImGui::Spacing();
                if (s.menu.heading) ImGui::PushFont(s.menu.heading);
                ImGui::TextUnformatted(w.text.c_str());
                if (s.menu.heading) ImGui::PopFont();
                if (!w.help.empty()) ImGui::TextDisabled("%s", w.help.c_str());
                break;
            case Kind::note: ImGui::TextDisabled("%s", w.text.c_str()); break;
            case Kind::warn: ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", w.text.c_str()); break;
            case Kind::info: ImGui::Text("%s: %s", w.text.c_str(), w.help.c_str()); break;
            case Kind::tabs:
                for (int i = 0; i < static_cast<int>(w.choices.size()); ++i) {
                    if (i) ImGui::SameLine();
                    ImGui::PushID(i);
                    if (ImGui::Selectable(w.choices[static_cast<std::size_t>(i)].c_str(), i + 1 == w.chosen, 0,
                                          ImGui::CalcTextSize(w.choices[static_cast<std::size_t>(i)].c_str()))) {
                        action.chosen = i + 1;
                        acted = true;
                    }
                    ImGui::PopID();
                }
                break;
            case Kind::bar:
                ImGui::ProgressBar((w.value - w.minimum) / (w.maximum - w.minimum), ImVec2(wide, 0), w.text.c_str());
                break;
            case Kind::button: acted = ImGui::Button(w.text.c_str(), ImVec2(button, 0)); break;
            case Kind::toggle: {
                bool on = w.on;
                if (ImGui::Checkbox(w.text.c_str(), &on)) {
                    action.on = on;
                    acted = true;
                }
                break;
            }
            case Kind::slider: {
                float value = w.value;
                ImGui::SetNextItemWidth(wide);
                if (ImGui::SliderFloat(w.text.c_str(), &value, w.minimum, w.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                    action.value = value;
                    acted = true;
                }
                break;
            }
            case Kind::dropdown: {
                int selected = std::clamp(w.chosen - 1, 0, static_cast<int>(w.choices.size()) - 1);
                ImGui::SetNextItemWidth(wide);
                if (ImGui::BeginCombo(w.text.c_str(), w.choices.empty() ? "" : w.choices[static_cast<std::size_t>(selected)].c_str())) {
                    for (int i = 0; i < static_cast<int>(w.choices.size()); ++i)
                        if (ImGui::Selectable(w.choices[static_cast<std::size_t>(i)].c_str(), i == selected)) {
                            action.chosen = i + 1;
                            acted = true;
                        }
                    ImGui::EndCombo();
                }
                break;
            }
            }
            if (acted) plugin_ui::act(std::move(action));
            ImGui::PopID();
        }
        // Esc closes it at once here; its plugin hears screen_closed.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput && !typing_taken()) {
            plugin_ui::act({screen->plugin, screen->title, plugin_ui::close_widget});
            closed_screen() = screen->plugin + '\n' + screen->title;
            s.plugin_screen_visible.store(false);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_plugins() {
    const auto layers = frame();
    if (!layers || layers->empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float k = display.y / 1080.0f;
    auto& s = detail::state();
    auto* fallback = ImGui::GetFont();
    const std::array<ImFont*, 5> fonts{s.menu.bold ? s.menu.bold : fallback, s.menu.body ? s.menu.body : fallback,
                                       s.menu.heading ? s.menu.heading : fallback, s.menu.title ? s.menu.title : fallback,
                                       s.menu.title_large ? s.menu.title_large : s.menu.title ? s.menu.title : fallback};
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto camera = plugin_camera::projector(display.x, display.y);
    // The fonts' advances for d:measure, again whenever the atlas is new.
    static const void* measured{};
    if (measured != ImGui::GetIO().Fonts) {
        measured = ImGui::GetIO().Fonts;
        for (int i = 0; i < 5; ++i) {
            auto* font = fonts[static_cast<std::size_t>(i)];
            plugin_ui::detail::FontMetrics m;
            m.size = font->FontSize;
            for (int c = 32; c < 128; ++c) m.advance[static_cast<std::size_t>(c - 32)] = font->GetCharAdvance(static_cast<ImWchar>(c));
            plugin_ui::set_font(i, m);
        }
    }
    using Kind = plugin_ui::Shape::Kind;
    for (const auto& layer : *layers) {
        int clips = 0;
        for (const auto& shape : layer.shapes) {
            const auto& style = shape.style;
            const auto& p = shape.points;
            auto* font = fonts[static_cast<std::size_t>(std::clamp(style.font, 0, 4))];
            switch (shape.kind) {
            case Kind::text: text(draw, font, k, at(p[0]), shape); break;
            case Kind::rect:
                if (style.fill) draw->AddRectFilled(at(p[0]), at(p[1]), style.colour, style.rounding * k);
                else draw->AddRect(at(p[0]), at(p[1]), style.colour, style.rounding * k, 0, style.thickness * k);
                break;
            case Kind::line: draw->AddLine(at(p[0]), at(p[1]), style.colour, style.thickness * k); break;
            case Kind::circle: circle(draw, at(p[0]), shape.radius, style, k); break;
            case Kind::poly: {
                std::vector<ImVec2> points;
                for (const auto& point : p) points.push_back(at(point));
                if (style.fill) draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), style.colour);
                else draw->AddPolyline(points.data(), static_cast<int>(points.size()), style.colour, ImDrawFlags_Closed, style.thickness * k);
                break;
            }
            case Kind::world_text:
                if (const auto on = camera ? camera->project(p[0]) : std::nullopt) text(draw, font, k, ImVec2((*on)[0], (*on)[1]), shape);
                break;
            case Kind::world_line: {
                const auto a = camera ? camera->project(p[0]) : std::nullopt;
                const auto b = camera ? camera->project(p[1]) : std::nullopt;
                if (a && b) draw->AddLine(ImVec2((*a)[0], (*a)[1]), ImVec2((*b)[0], (*b)[1]), style.colour, style.thickness * k);
                break;
            }
            case Kind::world_circle:
                if (const auto on = camera ? camera->project(p[0]) : std::nullopt)
                    circle(draw, ImVec2((*on)[0], (*on)[1]), std::min(shape.radius * camera->focal / (*on)[2], display.y), style, k);
                break;
            case Kind::image: {
                ImVec2 size;
                if (const auto id = detail::plugin_image(shape.text, size))
                    draw->AddImageRounded(id, at(p[0]), at(p[1]), ImVec2(style.uv[0], style.uv[1]), ImVec2(style.uv[2], style.uv[3]), style.colour,
                                          style.rounding * k);
                break;
            }
            case Kind::world_image: {
                ImVec2 size;
                const auto id = detail::plugin_image(shape.text, size);
                const auto on = camera ? camera->project(p[0]) : std::nullopt;
                if (!id || !on || size.y <= 0) break;
                const float span_x = std::abs(style.uv[2] - style.uv[0]), span_y = std::abs(style.uv[3] - style.uv[1]);
                const float aspect = span_y > 0 ? size.x * span_x / (size.y * span_y) : 1.0f;
                const float height = std::min(shape.radius * camera->focal / (*on)[2], display.y), width = height * aspect;
                draw->AddImage(id, ImVec2((*on)[0] - width * .5f, (*on)[1] - height * .5f), ImVec2((*on)[0] + width * .5f, (*on)[1] + height * .5f),
                               ImVec2(style.uv[0], style.uv[1]), ImVec2(style.uv[2], style.uv[3]), style.colour);
                break;
            }
            case Kind::game_image:
                (void)draw_game_image(draw, shape.text, at(p[0]), at(p[1]), style.colour, static_cast<GameImageFit>(std::clamp(shape.fit, 0, 2)),
                                      shape.edge * k, shape.frame);
                break;
            case Kind::gradient:
                if (shape.colours.size() == 4)
                    draw->AddRectFilledMultiColor(at(p[0]), at(p[1]), shape.colours[0], shape.colours[1], shape.colours[2], shape.colours[3]);
                break;
            case Kind::skeleton: {
                if (shape.colours.size() != skater_body::count) break;
                std::array<ImU32, skater_body::count> paint{};
                std::copy(shape.colours.begin(), shape.colours.end(), paint.begin());
                draw_xray(draw, paint, static_cast<float>(style.colour >> IM_COL32_A_SHIFT & 0xff) / 255.0f);
                break;
            }
            case Kind::rough_rect: skate_theme::rough_rect(draw, at(p[0]), at(p[1]), style.colour, shape.seed, shape.radius * k); break;
            case Kind::scribble: skate_theme::scribble(draw, at(p[0]), at(p[1]), shape.seed, shape.radius * k); break;
            case Kind::halftone: theme::halftone(draw, at(p[0]), shape.columns, shape.rows, style.colour); break;
            case Kind::clip:
                if (clips >= 16) break;
                draw->PushClipRect(at(p[0]), at(p[1]), true);
                ++clips;
                break;
            case Kind::unclip:
                if (clips > 0) {
                    draw->PopClipRect();
                    --clips;
                }
                break;
            }
        }
        for (; clips > 0; --clips) draw->PopClipRect();
    }
}
}
