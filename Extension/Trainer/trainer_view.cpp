#include "trainer.h"
#include <atomic>
#include <mutex>

// The snapshots the menu and HUD read. Kept apart from trainer.cpp so the overlay library
// links without the game-facing half.
namespace dingosdk::trainer {
namespace {
struct Feed {
    std::mutex mutex;
    std::shared_ptr<const View> view = std::make_shared<const View>();
    Telemetry telemetry;
    std::atomic<bool> class_list_shown{};
    std::atomic<bool> teleport_card_shown{};
};
Feed &feed() {
    static auto *value = new Feed;
    return *value;
}
} // namespace
std::shared_ptr<const View> view() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.view;
}
Telemetry telemetry() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.telemetry;
}
void publish(std::shared_ptr<const View> next) noexcept {
    if (!next) return;
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.view = std::move(next);
}
void publish(const Telemetry &next) noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.telemetry = next;
}
void note_class_list_shown() noexcept { feed().class_list_shown.store(true, std::memory_order_release); }
bool take_class_list_shown() noexcept { return feed().class_list_shown.exchange(false, std::memory_order_acq_rel); }
void note_teleport_card_shown() noexcept { feed().teleport_card_shown.store(true, std::memory_order_release); }
bool take_teleport_card_shown() noexcept { return feed().teleport_card_shown.exchange(false, std::memory_order_acq_rel); }
} // namespace dingosdk::trainer
