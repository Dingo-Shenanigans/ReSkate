#include "car_grab_hud.h"
#include "car_grab/controller.h"
#include "overlay_internal.h"
#include "skate_style.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dingosdk::car_grab_hud {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto lease_age = std::chrono::milliseconds(250);
struct Channel {
    std::mutex mutex;
    Feedback feedback;
    Clock::time_point published{};
    Feedback notice;
    Clock::time_point notice_until{};
    Clock::time_point idle_event{};
};
Channel& channel() { static auto* value = new Channel; return *value; }

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
bool camera_is_valid(const Feedback& value) {
    if (!value.camera_valid || !value.marker || !std::isfinite(value.vertical_fov) ||
        value.vertical_fov <= 1.f || value.vertical_fov >= 175.f) return false;
    for (const auto v : value.anchor)
        if (!std::isfinite(v) || std::abs(v) > 100000.f) return false;
    for (const auto v : value.camera)
        if (!std::isfinite(v) || std::abs(v) > 100000.f) return false;
    const auto& m = value.camera;
    const std::array<float, 3> right{m[0], m[1], m[2]}, up{m[4], m[5], m[6]}, back{m[8], m[9], m[10]};
    for (const auto* basis : {&right, &up, &back})
        if (std::abs(dot(*basis, *basis) - 1.f) > .12f) return false;
    return std::abs(dot(right, up)) < .08f && std::abs(dot(right, back)) < .08f && std::abs(dot(up, back)) < .08f;
}
Feedback sanitized(Feedback value) {
    if (value.phase < Phase::hidden || value.phase > Phase::towing ||
        value.cause < Cause::none || value.cause > Cause::vehicle_too_slow ||
        !std::isfinite(value.speed_mps) || value.speed_mps < 0.f || value.speed_mps > 100.f ||
        !std::isfinite(value.distance) || value.distance < 0.f || value.distance > 100000.f ||
        ((value.phase == Phase::available || value.phase == Phase::attached ||
          value.phase == Phase::approaching || value.phase == Phase::palm_contact ||
          value.phase == Phase::towing) && value.world == 0)) return {};
    // Kinematic towing makes no physical-contact claim or bumper projection.
    if (value.phase == Phase::towing) { value.marker = false; value.camera_valid = false; }
    if (!camera_is_valid(value)) { value.marker = false; value.camera_valid = false; }
    return value;
}
} // namespace

SearchFeedback search_feedback(const car_grab::TargetInfo& target) noexcept {
    using Status = car_grab::TargetStatus;
    // Explicit kinematic targets report rider/tether distance. A measured
    // target additionally requires its exact collision contact. Neither this
    // advisory renderer nor an unspecified mode can authorize attachment.
    const bool mode_valid = target.mode == car_grab::TowMode::kinematic ||
        (target.mode == car_grab::TowMode::measured_surface && target.grip.valid && target.grip.surface_id);
    const bool eligible = target.available && target.eligible && target.id && mode_valid &&
        std::isfinite(target.distance) && target.distance >= 0.f;
    if (eligible && target.status == Status::ready && target.in_reach)
        return {Phase::available, Cause::none};
    if (eligible && target.status == Status::out_of_range && !target.in_reach && target.distance > 0.f)
        return {Phase::seeking, Cause::none};
    switch (target.status) {
    case Status::no_vehicle: return {Phase::unavailable, Cause::car_not_detected};
    case Status::missing_surface: return {Phase::unavailable, Cause::surface_unavailable};
    case Status::too_slow: return {Phase::unavailable, Cause::vehicle_too_slow};
    case Status::none: return {Phase::unavailable, Cause::none};
    default: return {Phase::unavailable, Cause::vehicle_stale};
    }
}

void publish(Feedback feedback) noexcept {
    try {
        feedback = sanitized(feedback);
        auto& value = channel();
        // Rendering only copies this small value. Never stall a physics tick
        // if presentation happens to be reading it at the same instant.
        std::unique_lock lock(value.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        const auto now = Clock::now();
        const bool changed = value.feedback.phase != feedback.phase || value.feedback.cause != feedback.cause ||
            value.feedback.world != feedback.world;
        if (feedback.phase == Phase::hidden) value.idle_event = {};
        if (feedback.phase == Phase::idle &&
            (value.feedback.phase != Phase::idle || value.feedback.world != feedback.world))
            value.idle_event = now;
        if (value.feedback.world != feedback.world || feedback.phase == Phase::hidden || feedback.phase == Phase::attached ||
            feedback.phase == Phase::approaching || feedback.phase == Phase::palm_contact || feedback.phase == Phase::towing) {
            value.notice = {};
            value.notice_until = {};
        }
        if (changed && (feedback.phase == Phase::released ||
            (feedback.phase == Phase::blocked && feedback.cause != Cause::reset && feedback.cause != Cause::none))) {
            // Keep the event even if the next physics tick publishes idle
            // before Present reads. This never retains an attached claim.
            value.notice = feedback;
            value.notice_until = now + std::chrono::milliseconds(650);
        }
        value.feedback = feedback;
        value.published = now;
    } catch (...) {}
}
void clear() noexcept {
    try {
        auto& value = channel();
        std::lock_guard lock(value.mutex);
        value.feedback = {};
        value.published = {};
        value.notice = {};
        value.notice_until = {};
        value.idle_event = {};
    } catch (...) {}
}
} // namespace dingosdk::car_grab_hud

namespace dingosdk::overlay::detail {
namespace {
namespace hud = dingosdk::car_grab_hud;
namespace skate = dingosdk::skate_theme;
using Clock = std::chrono::steady_clock;
struct RenderState {
    hud::Feedback feedback;
    Clock::time_point published{}, appeared{}, changed{}, idle_since{};
    Clock::time_point idle_event{}, seen_idle_event{};
    hud::Feedback notice;
    Clock::time_point notice_until{};
    hud::Phase last_phase = hud::Phase::hidden;
    hud::Cause last_cause = hud::Cause::none;
    std::uint64_t world{};
};
RenderState& hud_state() { static RenderState value; return value; }
float seconds(Clock::time_point then) {
    return then == Clock::time_point{} ? 0.f : std::chrono::duration<float>(Clock::now() - then).count();
}
ImU32 faded(ImU32 colour, float alpha) {
    const auto a = static_cast<ImU32>(static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xffu) *
        std::clamp(alpha, 0.f, 1.f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
ImU32 accent(hud::Phase phase) {
    if (phase == hud::Phase::attached) return skate::good;
    if (phase == hud::Phase::blocked || phase == hud::Phase::unavailable) return skate::warning;
    if (phase == hud::Phase::available || phase == hud::Phase::seeking ||
        phase == hud::Phase::approaching || phase == hud::Phase::palm_contact || phase == hud::Phase::towing) return skate::blue;
    return skate::grey_text;
}
const char* title(const hud::Feedback& value) {
    switch (value.phase) {
    case hud::Phase::available: return "CAR IN REACH";
    case hud::Phase::seeking: return "GET CLOSER";
    case hud::Phase::attached: return "SKITCHING";
    case hud::Phase::towing: return "TOWING";
    case hud::Phase::approaching: return "REACHING";
    case hud::Phase::palm_contact: return "PALM PLANTED";
    case hud::Phase::released: return "LET GO";
    case hud::Phase::blocked:
        return value.cause == hud::Cause::reset ? "RELEASE TO RETRY" : "GRIP RELEASED";
    case hud::Phase::unavailable:
        if (value.cause == hud::Cause::surface_unavailable) return "BUMPER NOT FOUND";
        if (value.cause == hud::Cause::car_not_detected) return "NO CAR FOUND";
        if (value.cause == hud::Cause::vehicle_too_slow) return "CAR TOO SLOW";
        return "WAITING FOR TRAFFIC";
    default: return "CAR GRAB";
    }
}
const char* stage(hud::Phase phase) {
    switch (phase) {
    case hud::Phase::approaching: return "HAND REACH";
    case hud::Phase::palm_contact: return "PALM CONTACT";
    case hud::Phase::attached: return "FULL GRIP";
    case hud::Phase::towing: return "CAR ATTACHED";
    default: return "CAR GRAB";
    }
}
const char* reason(hud::Cause cause) {
    switch (cause) {
    case hud::Cause::braked: return "Braking releases your grip";
    case hud::Cause::jumped: return "Ollie releases your grip";
    case hud::Cause::out_of_reach: return "The car moved out of reach";
    case hud::Cause::car_disappeared: return "The car is no longer available";
    case hud::Cause::vehicle_stale: return "Waiting for the car's next update";
    case hud::Cause::reset: return "Release the grab button, then try again";
    case hud::Cause::not_on_board: return "Get on your board to grab a car";
    case hud::Cause::controls_busy: return "Return to skating to grab a car";
    case hud::Cause::car_not_detected: return "Ride behind a traffic car";
    case hud::Cause::surface_unavailable: return "Unable to detect a grab point on this car";
    case hud::Cause::vehicle_too_slow: return "The car needs to be moving";
    default: return "Release the grab button, then try again";
    }
}
void car_icon(ImDrawList* draw, ImVec2 at, float k, ImU32 colour) {
    const ImVec2 body[]{
        {at.x - 13.f * k, at.y + 3.f * k}, {at.x - 12.f * k, at.y - 3.f * k},
        {at.x - 7.f * k, at.y - 4.f * k}, {at.x - 3.f * k, at.y - 9.f * k},
        {at.x + 5.f * k, at.y - 9.f * k}, {at.x + 9.f * k, at.y - 4.f * k},
        {at.x + 13.f * k, at.y - 2.f * k}, {at.x + 13.f * k, at.y + 3.f * k}};
    draw->AddPolyline(body, 8, colour, ImDrawFlags_Closed, 1.8f * k);
    draw->AddLine({at.x - 7.f * k, at.y - 4.f * k}, {at.x + 9.f * k, at.y - 4.f * k}, colour, 1.5f * k);
    draw->AddCircleFilled({at.x - 8.f * k, at.y + 4.f * k}, 3.f * k, colour, 12);
    draw->AddCircleFilled({at.x + 8.f * k, at.y + 4.f * k}, 3.f * k, colour, 12);
}
void hand_icon(ImDrawList* draw, ImVec2 at, float k, ImU32 colour, bool closed) {
    if (closed) {
        const ImVec2 outline[]{
            {at.x - 7.f * k, at.y + 12.f * k}, {at.x - 8.f * k, at.y + 3.f * k},
            {at.x - 12.f * k, at.y - 1.f * k}, {at.x - 12.f * k, at.y - 6.f * k},
            {at.x - 9.f * k, at.y - 8.f * k}, {at.x - 5.f * k, at.y - 7.f * k},
            {at.x - 5.f * k, at.y - 11.f * k}, {at.x + 1.f * k, at.y - 11.f * k},
            {at.x + 1.f * k, at.y - 10.f * k}, {at.x + 6.f * k, at.y - 10.f * k},
            {at.x + 6.f * k, at.y - 8.f * k}, {at.x + 10.f * k, at.y - 8.f * k},
            {at.x + 12.f * k, at.y - 4.f * k}, {at.x + 11.f * k, at.y + 4.f * k},
            {at.x + 6.f * k, at.y + 12.f * k}};
        draw->AddPolyline(outline, 15, colour, ImDrawFlags_Closed, 1.8f * k);
        draw->AddLine({at.x - 5.f * k, at.y - 7.f * k}, {at.x - 5.f * k, at.y - 2.f * k}, colour, 1.6f * k);
        draw->AddLine({at.x + 1.f * k, at.y - 10.f * k}, {at.x + 1.f * k, at.y - 3.f * k}, colour, 1.6f * k);
        draw->AddLine({at.x + 6.f * k, at.y - 8.f * k}, {at.x + 6.f * k, at.y - 3.f * k}, colour, 1.6f * k);
        draw->AddLine({at.x - 7.f * k, at.y + 1.f * k}, {at.x + 5.f * k, at.y + 2.f * k}, colour, 1.8f * k);
        return;
    }
    const ImVec2 outline[]{
        {at.x - 6.f * k, at.y + 12.f * k}, {at.x - 7.f * k, at.y + 5.f * k},
        {at.x - 12.f * k, at.y - 1.f * k}, {at.x - 12.f * k, at.y - 4.f * k},
        {at.x - 9.f * k, at.y - 5.f * k}, {at.x - 6.f * k, at.y - 1.f * k},
        {at.x - 6.f * k, at.y - 12.f * k}, {at.x - 3.f * k, at.y - 12.f * k},
        {at.x - 3.f * k, at.y - 4.f * k}, {at.x - 1.f * k, at.y - 4.f * k},
        {at.x - 1.f * k, at.y - 16.f * k}, {at.x + 2.f * k, at.y - 16.f * k},
        {at.x + 2.f * k, at.y - 4.f * k}, {at.x + 4.f * k, at.y - 4.f * k},
        {at.x + 4.f * k, at.y - 13.f * k}, {at.x + 7.f * k, at.y - 13.f * k},
        {at.x + 7.f * k, at.y - 3.f * k}, {at.x + 9.f * k, at.y - 3.f * k},
        {at.x + 9.f * k, at.y - 8.f * k}, {at.x + 12.f * k, at.y - 8.f * k},
        {at.x + 12.f * k, at.y + 3.f * k}, {at.x + 7.f * k, at.y + 12.f * k}};
    draw->AddPolyline(outline, 22, colour, ImDrawFlags_Closed, 1.65f * k);
    draw->AddLine({at.x - 4.f * k, at.y + 5.f * k}, {at.x + 6.f * k, at.y + 5.f * k}, colour, 1.4f * k);
}
void state_icon(ImDrawList* draw, ImVec2 at, float k, ImU32 colour, hud::Phase phase) {
    switch (phase) {
    case hud::Phase::attached:
        hand_icon(draw, at, k, colour, true);
        break;
    case hud::Phase::approaching:
        hand_icon(draw, at, k, colour, false);
        break;
    case hud::Phase::palm_contact:
        hand_icon(draw, {at.x - 4.f * k, at.y}, .86f * k, colour, false);
        draw->AddRectFilled({at.x + 13.f * k, at.y - 16.f * k}, {at.x + 16.f * k, at.y + 15.f * k}, colour, k);
        draw->AddLine({at.x + 8.f * k, at.y}, {at.x + 13.f * k, at.y}, colour, 1.8f * k);
        break;
    case hud::Phase::released:
        hand_icon(draw, {at.x - 3.f * k, at.y}, .8f * k, colour, false);
        draw->AddLine({at.x + 8.f * k, at.y + 5.f * k}, {at.x + 17.f * k, at.y + 5.f * k}, colour, 1.8f * k);
        draw->AddLine({at.x + 13.f * k, at.y + 1.f * k}, {at.x + 17.f * k, at.y + 5.f * k}, colour, 1.8f * k);
        draw->AddLine({at.x + 13.f * k, at.y + 9.f * k}, {at.x + 17.f * k, at.y + 5.f * k}, colour, 1.8f * k);
        break;
    case hud::Phase::blocked:
        hand_icon(draw, at, .8f * k, colour, false);
        draw->AddLine({at.x - 17.f * k, at.y + 16.f * k}, {at.x + 17.f * k, at.y - 17.f * k}, colour, 2.f * k);
        break;
    case hud::Phase::unavailable:
        draw->AddRectFilled({at.x - 8.f * k, at.y - 11.f * k}, {at.x - 3.f * k, at.y + 11.f * k}, colour, k);
        draw->AddRectFilled({at.x + 3.f * k, at.y - 11.f * k}, {at.x + 8.f * k, at.y + 11.f * k}, colour, k);
        break;
    default:
        car_icon(draw, at, k, colour);
        break;
    }
}
float draw_keycap(ImDrawList* draw, ImFont* font, ImVec2 at, const char* cap, float k, float alpha) {
    const auto text = font->CalcTextSizeA(14.f * k, FLT_MAX, 0.f, cap);
    const float width = std::max(30.f * k, text.x + 18.f * k), height = 28.f * k;
    draw->AddRectFilled({at.x, at.y + 2.f * k}, {at.x + width, at.y + height + 2.f * k}, faded(skate::tile_light, alpha), 4.f * k);
    draw->AddRectFilled(at, {at.x + width, at.y + height}, faded(theme::paper, alpha), 4.f * k);
    draw->AddText(font, 14.f * k, {at.x + (width - text.x) * .5f, at.y + (height - text.y) * .5f}, faded(skate::black, alpha), cap);
    return width;
}
bool project(const hud::Feedback& value, ImVec2 display, ImVec2& point) {
    if (!value.marker || !value.camera_valid) return false;
    const auto& m = value.camera;
    const std::array<float, 3> delta{value.anchor[0] - m[12], value.anchor[1] - m[13], value.anchor[2] - m[14]};
    const std::array<float, 3> right{m[0], m[1], m[2]}, up{m[4], m[5], m[6]}, back{m[8], m[9], m[10]};
    const float depth = -hud::dot(delta, back);
    if (!std::isfinite(depth) || depth <= .1f) return false;
    const float focal = display.y / (2.f * std::tan(value.vertical_fov * 3.14159265f / 360.f));
    point = {display.x * .5f + hud::dot(delta, right) * focal / depth,
             display.y * .5f - hud::dot(delta, up) * focal / depth};
    return std::isfinite(point.x) && std::isfinite(point.y) && point.x >= 20.f && point.x <= display.x - 20.f &&
        point.y >= 20.f && point.y <= display.y - 20.f;
}
void rear_marker(ImDrawList* draw, const hud::Feedback& value, ImVec2 display, float k, float alpha) {
    if (value.phase != hud::Phase::available && value.phase != hud::Phase::attached &&
        value.phase != hud::Phase::approaching && value.phase != hud::Phase::palm_contact) return;
    ImVec2 point;
    if (!project(value, display, point)) return;
    const ImU32 colour = faded(accent(value.phase), alpha);
    // Marker shape repeats the producer's current state. There is no invented
    // fill percentage: an open hand, planted palm and closed grip are distinct.
    const float settle = std::clamp(seconds(hud_state().changed) / .18f, 0.f, 1.f);
    const float r = (value.phase == hud::Phase::attached ? 17.f + (1.f - settle) * 4.f : 21.f) * k;
    draw->AddCircleFilled(point, r + 3.f * k, faded(theme::ink, .6f * alpha), 32);
    if (value.phase == hud::Phase::available) {
        draw->AddCircle(point, r - 3.f * k, colour, 32, 1.5f * k);
        draw->AddCircleFilled(point, 2.5f * k, faded(theme::paper, alpha), 12);
    } else {
        if (value.phase == hud::Phase::attached)
            draw->AddCircle(point, r, colour, 32, 2.f * k);
        state_icon(draw, point, .65f * k, faded(theme::paper, alpha), value.phase);
    }
    for (const auto direction : {ImVec2{-1.f, -1.f}, ImVec2{1.f, -1.f}, ImVec2{-1.f, 1.f}, ImVec2{1.f, 1.f}}) {
        const ImVec2 corner{point.x + direction.x * (r + 5.f * k), point.y + direction.y * (r + 5.f * k)};
        draw->AddLine(corner, {corner.x - direction.x * 6.f * k, corner.y}, colour, 2.f * k);
        draw->AddLine(corner, {corner.x, corner.y - direction.y * 6.f * k}, colour, 2.f * k);
    }
}
} // namespace

bool car_grab_hud_pending() {
    auto& shown = hud_state();
    auto& source = hud::channel();
    {
        std::unique_lock lock(source.mutex, std::try_to_lock);
        if (lock.owns_lock()) {
            shown.feedback = source.feedback;
            shown.published = source.published;
            shown.notice = source.notice;
            shown.notice_until = source.notice_until;
            shown.idle_event = source.idle_event;
        }
    }
    const auto now = Clock::now();
    if (interactive_visible(state()) || shown.published == Clock::time_point{} ||
        now < shown.published || now - shown.published > hud::lease_age || shown.feedback.phase == hud::Phase::hidden) {
        shown = {};
        return false;
    }
    const auto incoming_phase = shown.feedback.phase;
    const auto incoming_cause = shown.feedback.cause;
    if (shown.last_phase == hud::Phase::hidden || shown.world != shown.feedback.world)
        shown.appeared = now;
    const bool changed = shown.last_phase != incoming_phase || shown.last_cause != incoming_cause || shown.world != shown.feedback.world;
    if (changed)
        shown.changed = now;
    if (incoming_phase == hud::Phase::idle && (shown.last_phase != hud::Phase::idle ||
        shown.world != shown.feedback.world || shown.idle_event != shown.seen_idle_event))
        shown.idle_since = now;
    shown.seen_idle_event = shown.idle_event;
    const bool ordinary = incoming_phase == hud::Phase::idle || incoming_phase == hud::Phase::available ||
        incoming_phase == hud::Phase::seeking;
    if (now < shown.notice_until && (ordinary || (incoming_phase == hud::Phase::blocked && incoming_cause == hud::Cause::reset))) {
        // Keep only the release/reason text readable across faster physics
        // ticks. Lease expiry, menu/focus suppression and new attachments still
        // take effect immediately; an attached claim is never held this way.
        shown.feedback.phase = shown.notice.phase;
        shown.feedback.cause = shown.notice.cause;
        shown.feedback.marker = false;
    }
    shown.last_phase = incoming_phase;
    shown.last_cause = incoming_cause;
    shown.world = shown.feedback.world;
    // Keep the control hint discoverable after entry or an ended attempt,
    // then stop submitting idle UI frames until there is something actionable.
    return shown.feedback.phase != hud::Phase::idle || seconds(shown.idle_since) < 4.f;
}

void draw_car_grab_hud() {
    const auto& shown = hud_state();
    const auto& feedback = shown.feedback;
    if (feedback.phase == hud::Phase::hidden || interactive_visible(state()) ||
        shown.published == Clock::time_point{} || Clock::now() - shown.published > hud::lease_age) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (!std::isfinite(display.x) || !std::isfinite(display.y) || display.x < 320.f || display.y < 240.f) return;
    const float k = std::clamp(display.y / 1080.f, .8f, 2.f);
    const float alpha = std::clamp(seconds(shown.appeared) / .12f, 0.f, 1.f);
    auto& s = state();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* bold = s.menu.bold ? s.menu.bold : body;
    auto* heading = s.menu.heading ? s.menu.heading : bold;
    auto* draw = ImGui::GetBackgroundDrawList();
    rear_marker(draw, feedback, display, k, alpha);

    const char* cap = feedback.controller ? "RB/R1" : "G";
    if (feedback.phase == hud::Phase::idle) {
        const float idle_age = seconds(shown.idle_since);
        if (idle_age >= 4.f) return;
        const float hint_alpha = alpha * std::clamp((4.f - idle_age) / .5f, 0.f, 1.f);
        // An input legend, rather than a permanent status panel. It returns
        // after an ended attempt, menu return or world change.
        const auto label = body->CalcTextSizeA(14.f * k, FLT_MAX, 0.f, "Grab a traffic car");
        const float cap_width = std::max(30.f * k, bold->CalcTextSizeA(14.f * k, FLT_MAX, 0.f, cap).x + 18.f * k);
        const float width = std::min(cap_width + label.x + 36.f * k, display.x - 32.f), height = 36.f * k;
        const ImVec2 at{(display.x - width) * .5f, display.y - 108.f * k - height};
        draw->AddRectFilled(at, {at.x + width, at.y + height}, faded(theme::ink, .7f * hint_alpha), 5.f * k);
        const ImVec2 cap_at{at.x + 4.f * k, at.y + 4.f * k};
        draw_keycap(draw, bold, cap_at, cap, k, hint_alpha);
        const ImVec2 label_at{cap_at.x + cap_width + 12.f * k, at.y + (height - label.y) * .5f};
        draw->PushClipRect(label_at, {at.x + width - 8.f * k, at.y + height}, true);
        draw->AddText(body, 14.f * k, label_at, faded(theme::paper, hint_alpha), "Grab a traffic car");
        draw->PopClipRect();
        return;
    }

    const float width = std::min(520.f * k, display.x - 32.f), height = 100.f * k;
    const ImVec2 at{(display.x - width) * .5f, display.y - 108.f * k - height};
    const ImVec2 end{at.x + width, at.y + height};
    const ImU32 colour = faded(accent(feedback.phase), alpha);
    draw->AddRectFilled({at.x, at.y + 4.f * k}, {end.x, end.y + 4.f * k}, faded(skate::black, .3f * alpha), 7.f * k);
    draw->AddRectFilled(at, end, faded(theme::ink, .95f * alpha), 7.f * k);
    draw->AddRectFilled({at.x, at.y + 10.f * k}, {at.x + 3.f * k, end.y - 10.f * k}, colour, 1.5f * k);
    const ImVec2 icon{at.x + 35.f * k, at.y + 49.f * k};
    draw->AddRectFilled({icon.x - 23.f * k, icon.y - 23.f * k}, {icon.x + 23.f * k, icon.y + 23.f * k},
        faded(skate::tile_light, .45f * alpha), 7.f * k);
    state_icon(draw, icon, k, colour, feedback.phase);

    const ImVec2 text_at{at.x + 70.f * k, at.y + 27.f * k};
    const float cap_width = std::max(30.f * k, bold->CalcTextSizeA(14.f * k, FLT_MAX, 0.f, cap).x + 18.f * k);
    const ImVec2 cap_at{end.x - cap_width - 16.f * k, at.y + 21.f * k};
    const char* action = feedback.phase == hud::Phase::blocked ? "RELEASE" : "HOLD";
    const auto action_size = body->CalcTextSizeA(10.5f * k, FLT_MAX, 0.f, action);
    draw->AddText(body, 10.5f * k, {cap_at.x + (cap_width - action_size.x) * .5f, at.y + 7.f * k}, faded(theme::muted, alpha), action);
    draw_keycap(draw, bold, cap_at, cap, k, alpha);
    const float heading_width = cap_at.x - text_at.x - 12.f * k;
    const auto heading_size = heading->CalcTextSizeA(22.f * k, FLT_MAX, 0.f, title(feedback));
    const float heading_font_size = heading_size.x > heading_width
        ? std::max(16.f * k, 22.f * k * heading_width / heading_size.x) : 22.f * k;
    draw->PushClipRect({text_at.x, at.y + 9.f * k}, {text_at.x + heading_width, text_at.y + 24.f * k}, true);
    draw->AddText(bold, 11.5f * k, {text_at.x, at.y + 10.f * k}, colour, stage(feedback.phase));
    draw->AddText(heading, heading_font_size, text_at, faded(theme::paper, alpha), title(feedback));
    draw->PopClipRect();

    const bool metric = feedback.phase == hud::Phase::attached || feedback.phase == hud::Phase::palm_contact ||
        feedback.phase == hud::Phase::towing;
    float line_right = end.x - 16.f * k;
    if (metric) {
        char speed[32]{};
        std::snprintf(speed, sizeof speed, "%.0f", feedback.speed_mps * 3.6f);
        const auto number = bold->CalcTextSizeA(22.f * k, FLT_MAX, 0.f, speed);
        const auto unit = body->CalcTextSizeA(10.5f * k, FLT_MAX, 0.f, "KM/H");
        const float metric_width = number.x + unit.x + 6.f * k;
        const ImVec2 metric_at{end.x - 16.f * k - metric_width, at.y + 68.f * k};
        draw->AddText(body, 10.5f * k, {metric_at.x, at.y + 54.f * k}, faded(theme::muted, alpha), "CAR SPEED");
        draw->AddText(bold, 22.f * k, metric_at, faded(theme::paper, alpha), speed);
        draw->AddText(body, 10.5f * k, {metric_at.x + number.x + 6.f * k, metric_at.y + number.y - unit.y}, faded(theme::muted, alpha), "KM/H");
        line_right = metric_at.x - 18.f * k;
    }

    char subtitle[192]{};
    switch (feedback.phase) {
    case hud::Phase::attached:
    case hud::Phase::towing:
        std::snprintf(subtitle, sizeof subtitle, "Release to let go. Steer with %s.", feedback.controller ? "left stick" : "A/D");
        break;
    case hud::Phase::approaching:
        std::snprintf(subtitle, sizeof subtitle, "Keep holding to reach the bumper.");
        break;
    case hud::Phase::palm_contact:
        std::snprintf(subtitle, sizeof subtitle, "Palm on bumper. Release to let go.");
        break;
    case hud::Phase::available:
        std::snprintf(subtitle, sizeof subtitle, "Hold to grab. Steer with %s.", feedback.controller ? "left stick" : "A/D");
        break;
    case hud::Phase::seeking:
        std::snprintf(subtitle, sizeof subtitle, "Move closer behind the car");
        break;
    case hud::Phase::released:
        std::snprintf(subtitle, sizeof subtitle, "Momentum kept. Hold to grab again.");
        break;
    case hud::Phase::blocked:
    case hud::Phase::unavailable:
        std::snprintf(subtitle, sizeof subtitle, "%s", reason(feedback.cause));
        break;
    default:
        std::snprintf(subtitle, sizeof subtitle, "Hold %s behind a traffic car", cap);
        break;
    }
    const ImVec2 line_at{text_at.x, at.y + 58.f * k};
    draw->PushClipRect(line_at, {line_right, end.y - 8.f * k}, true);
    draw->AddText(body, 15.f * k, line_at, faded(theme::paper, .9f * alpha), subtitle, nullptr,
        std::max(1.f, line_right - line_at.x));
    draw->PopClipRect();
}
} // namespace dingosdk::overlay::detail
