#include "Extension/Skater/first_person_settings_host.h"
#include "Extension/UI/Overlay/overlay.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dingosdk;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Queue {
    overlay::DebugRequest request;
    int calls{};
    bool accept{true};
    static bool submit(void* user, const overlay::DebugRequest& request, char*, std::size_t) {
        auto& self = *static_cast<Queue*>(user);
        self.request = request; ++self.calls; return self.accept;
    }
};
int main() { try {
    overlay::Model model;
    model.debug.available = model.debug.camera_available = true;
    model.debug.first_person = true;
    model.debug.first_person_fov = 87;
    auto& native = model.debug.first_person_arm;
    native.offset = {.1f, .2f, .3f}; native.rotation = {-10, 12, 3};
    native.up = 11; native.down = 22; native.left = 33; native.right = 44;
    native.board_only = native.camera_cycle = true;
    native.stabilize = false; native.follow_flips = false;
    native.smoothing = 61; native.head_pitch = 72; native.head_roll = 13; native.bob = 24;
    auto snapshot = first_person_host::read(model);
    check(snapshot.enabled && snapshot.available && snapshot.fov == 87 && snapshot.board_only, "Native master/FOV/board-only not read");
    check(snapshot.offset == native.offset && snapshot.rotation == native.rotation && snapshot.strength == std::array<float,4>{11,22,33,44}, "Spring values lost in conversion");
    check(!snapshot.stabilize && !snapshot.follow_flips && snapshot.camera_cycle && snapshot.bob == 24 && snapshot.head_pitch == 72, "ReSkate-only preferences lost");
    model.debug.first_person_fov = 101;
    check(first_person_host::read(model).fov == 101, "Host retained stale private preference");
    Queue queue; overlay::CallbacksV3 callbacks; callbacks.user = &queue; callbacks.queue_debug = Queue::submit;
    char error[128]{};
    using Field = first_person_host::Field;
    check(first_person_host::queue(callbacks, Field::spring, 1, error, sizeof(error)), "Spring request failed");
    check(queue.request.action == overlay::DebugAction::set_first_person_spring && queue.request.enabled, "Spring used wrong native action");
    check(first_person_host::queue(callbacks, Field::board_only, 0, error, sizeof(error)), "Board-only request failed");
    check(queue.request.action == overlay::DebugAction::set_first_person_board_only && !queue.request.enabled, "Board-only used duplicate state");
    check(first_person_host::queue(callbacks, Field::fov, 95, nullptr, 0), "Null feedback buffer was not supported");
    check(queue.request.value == 95 && model.debug.first_person_fov == 101, "Request mutated the read model instead of queuing");
    check(first_person_host::queue(callbacks, Field::fov, 0, error, sizeof(error)) && queue.request.value == 0,
          "Default FOV sentinel did not use the native queue");
    const auto calls = queue.calls;
    check(!first_person_host::queue(callbacks, Field::fov, 20, error, sizeof(error)), "Invalid FOV below native range accepted");
    check(!first_person_host::queue(callbacks, Field::spring, .5f, error, sizeof(error)), "Fractional boolean accepted");
    check(!first_person_host::queue(callbacks, Field::yaw, std::numeric_limits<float>::quiet_NaN(), error, sizeof(error)), "NaN accepted");
    check(!first_person_host::queue(callbacks, static_cast<Field>(999), 1, error, sizeof(error)), "Unknown field accepted");
    check(queue.calls == calls, "Invalid request reached the engine queue");
    queue.accept = false;
    check(!first_person_host::queue(callbacks, Field::enabled, 1, error, sizeof(error)), "Queue rejection was hidden");
    callbacks.queue_debug = nullptr;
    check(!first_person_host::queue(callbacks, Field::enabled, 1, error, sizeof(error)) && error[0], "Missing host reported success");
    std::cout << "ReSkate-owned settings, layout conversion and queue boundary passed.\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
