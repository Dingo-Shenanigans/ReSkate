#include "overlay_internal.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "reskate_version.h"

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {

// Corner notices, kept apart from the graphics state so they can be queued
// during startup, long before a swapchain exists.
struct Notice {
    dingosdk::overlay::NoticeLevel level{};
    std::string title, text;
    std::chrono::steady_clock::time_point start{};
};
std::mutex notices_mutex;
std::deque<Notice> notices;
constexpr std::size_t maximum_notices = 6;

constexpr std::uint64_t cover_fade_ms = 300;
std::atomic<std::uint64_t> cover_until{}, cover_started{};
std::mutex cover_mutex;
std::string cover_text;
bool cover_pending() { return GetTickCount64() < cover_until.load(std::memory_order_relaxed) + cover_fade_ms; }
float cover_alpha(std::uint64_t now) {
    const auto until = cover_until.load(std::memory_order_relaxed), started = cover_started.load(std::memory_order_relaxed);
    if (now >= until + cover_fade_ms) return 0;
    const float in = std::clamp(static_cast<float>(now - std::min(now, started)) / cover_fade_ms, 0.0f, 1.0f);
    const float out = now < until ? 1.0f : 1.0f - static_cast<float>(now - until) / cover_fade_ms;
    return std::min(in, out);
}
std::atomic<std::uint64_t> banner_until{};
std::string banner_text; // guarded by cover_mutex
bool banner_pending() { return GetTickCount64() < banner_until.load(std::memory_order_relaxed); }
}
void dingosdk::overlay::cover(std::string text, unsigned milliseconds) noexcept {
    try {
        std::lock_guard lock(cover_mutex);
        const auto now = GetTickCount64();
        const bool showing = cover_pending();
        if (!milliseconds) {
            if (showing) cover_until.store(std::min(cover_until.load(std::memory_order_relaxed), now), std::memory_order_relaxed);
            return;
        }
        cover_text = std::move(text);
        // A cover that is fading out fades back in from its current alpha.
        if (!showing) cover_started.store(now, std::memory_order_relaxed);
        else if (now >= cover_until.load(std::memory_order_relaxed))
            cover_started.store(now - static_cast<std::uint64_t>(cover_alpha(now) * cover_fade_ms), std::memory_order_relaxed);
        cover_until.store(now + milliseconds, std::memory_order_relaxed);
    } catch (...) {}
}
bool dingosdk::overlay::covered() noexcept { return cover_pending(); }
void dingosdk::overlay::banner(std::string text, unsigned milliseconds) noexcept {
    try {
        std::lock_guard lock(cover_mutex);
        banner_text = std::move(text);
        banner_until.store(milliseconds ? GetTickCount64() + milliseconds : 0, std::memory_order_relaxed);
    } catch (...) {}
}
namespace dingosdk::overlay::detail {
namespace {
// A dark band from the top of the screen to `bottom`, with `text` centred in it.
void draw_band(const std::string& text, float bottom, float text_height, int background_alpha, int text_alpha) {
    const auto width = ImGui::GetIO().DisplaySize.x;
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({0, 0}, {width, bottom}, IM_COL32(10, 10, 12, background_alpha));
    const auto extent = ImGui::GetFont()->CalcTextSizeA(text_height, FLT_MAX, 0.0f, text.c_str());
    draw->AddText(ImGui::GetFont(), text_height, {(width - extent.x) / 2, (bottom - extent.y) / 2}, IM_COL32(235, 235, 235, text_alpha),
                  text.c_str());
}
std::string copy_text(const std::string& text) {
    std::lock_guard lock(cover_mutex);
    return text;
}
}
void draw_banner() {
    if (!banner_pending() || cover_pending()) return;
    const float height = std::max(20.0f, ImGui::GetIO().DisplaySize.y * 0.03f);
    draw_band(copy_text(banner_text), height * 2.2f, height, 200, 255);
}
void draw_cover() {
    const float alpha = cover_alpha(GetTickCount64());
    if (alpha <= 0) return;
    const auto screen = ImGui::GetIO().DisplaySize.y;
    const auto shade = static_cast<int>(alpha * 255);
    draw_band(copy_text(cover_text), screen, std::max(22.0f, screen * 0.035f), shade, shade);
}
bool notices_pending() {
    if (cover_pending() || banner_pending()) return true;
    std::lock_guard lock(notices_mutex);
    return !notices.empty();
}

// Notices in the top-left corner: "ReSkate loaded" the first time the overlay
// draws, then whatever notify() queued (a broken mod, a map that is not
// loading), stacked and fading out on their own. They take no input.
void draw_notices() {
    using namespace std::chrono_literals;
    using dingosdk::overlay::NoticeLevel;
    constexpr auto fade_in = 250ms, fade_out = 700ms;
    auto& s = state();
    if (!s.loaded_notice_posted) {
        s.loaded_notice_posted = true;
        // "ReSkate 0.6.0 loaded" and the keys the launcher chose for the menu and console.
        const auto keys = dingosdk::launcher::overlay_keys();
        const auto name = [](unsigned key) { return key == VK_OEM_3 ? std::string("~") : dingosdk::launcher::key_name(key); };
        dingosdk::overlay::notify(NoticeLevel::info,
            dingosdk::reskate_release_build ? "ReSkate " + std::string(dingosdk::reskate_version) + " loaded"
                                            : std::string("ReSkate loaded (development build)"),
            "Press " + name(keys.menu) + " for the menu and " + name(keys.console) + " for the console.");
    }
    std::lock_guard lock(notices_mutex);
    if (notices.empty()) return;
    const auto now = std::chrono::steady_clock::now();

    namespace theme = dingosdk::overlay::theme;
    auto* heading = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const float scale = std::clamp(ImGui::GetIO().DisplaySize.y / 1080.0f, 1.0f, 2.0f);
    const float heading_size = heading->FontSize * scale, body_size = body->FontSize * scale * 0.9f;
    const float strip = 4.0f * scale, pad_x = 14.0f * scale, pad_y = 9.0f * scale, gap = 8.0f * scale;
    const float wrap = 520.0f * scale, between = 4.0f * scale;
    ImVec2 at(20.0f * scale, 20.0f * scale);
    auto* draw = ImGui::GetForegroundDrawList();
    for (auto it = notices.begin(); it != notices.end();) {
        auto& notice = *it;
        if (notice.start == std::chrono::steady_clock::time_point{}) notice.start = now;
        // Longer text stays up longer, so it can be read.
        const auto reading = std::chrono::milliseconds(std::min<std::size_t>(notice.text.size(), 200) * 40);
        const auto hold = (notice.level == NoticeLevel::info ? 3500ms
                         : notice.level == NoticeLevel::warning ? 12000ms : 20000ms) + reading;
        const auto age = now - notice.start;
        if (age >= hold + fade_out) { it = notices.erase(it); continue; }
        float alpha = std::min(1.0f, std::chrono::duration<float>(age) / fade_in);
        if (age > hold) alpha = std::min(alpha, 1.0f - std::chrono::duration<float>(age - hold) / fade_out);
        const auto faded = [alpha](ImU32 colour, float opacity = 1.0f) {
            const auto base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            return (colour & ~IM_COL32_A_MASK) |
                (static_cast<ImU32>(base * opacity * std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
        };
        const ImU32 accent = notice.level == NoticeLevel::info ? theme::blue
                           : notice.level == NoticeLevel::warning ? dingosdk::skate_theme::warning
                           : dingosdk::skate_theme::danger;
        const ImVec2 title_extent = heading->CalcTextSizeA(heading_size, FLT_MAX, wrap, notice.title.c_str());
        const ImVec2 text_extent = notice.text.empty() ? ImVec2(0.0f, 0.0f)
            : body->CalcTextSizeA(body_size, FLT_MAX, wrap, notice.text.c_str());
        const float width = std::max(title_extent.x, text_extent.x);
        const float height = title_extent.y + (notice.text.empty() ? 0.0f : between + text_extent.y);
        // Slides in a little as it fades in.
        const float slide = (1.0f - std::min(1.0f, std::chrono::duration<float>(age) / fade_in)) * -12.0f * scale;
        const ImVec2 top_left(at.x + slide, at.y);
        const ImVec2 bottom_right(top_left.x + strip + pad_x * 2.0f + width, top_left.y + pad_y * 2.0f + height);
        draw->AddRectFilled(top_left, bottom_right, faded(theme::ink, 0.92f));
        draw->AddRectFilled(top_left, ImVec2(top_left.x + strip, bottom_right.y), faded(accent));
        const ImVec2 text_at(top_left.x + strip + pad_x, top_left.y + pad_y);
        draw->AddText(heading, heading_size, text_at, faded(theme::paper), notice.title.c_str(), nullptr, wrap);
        if (!notice.text.empty())
            draw->AddText(body, body_size, ImVec2(text_at.x, text_at.y + title_extent.y + between),
                          faded(theme::paper, 0.85f), notice.text.c_str(), nullptr, wrap);
        at.y = bottom_right.y + gap;
        ++it;
    }
}
}

void dingosdk::overlay::notify(NoticeLevel level, std::string title, std::string text) noexcept {
    try {
        std::lock_guard lock(notices_mutex);
        while (notices.size() >= maximum_notices) notices.pop_front();
        notices.push_back({level, std::move(title), std::move(text), {}});
    } catch (...) {}
}
