#include "stand_ins.h"
#include <atomic>
#include <mutex>

namespace dingosdk::multiplayer::stand_ins {
namespace {
std::mutex mutex;
std::shared_ptr<const Policy> policy;
std::atomic<std::uint64_t> changes{1};
} // namespace
std::shared_ptr<const Policy> current() {
    std::lock_guard lock(mutex);
    return policy;
}
std::uint64_t generation() { return changes.load(std::memory_order_acquire); }
std::string set(Policy next) {
    // The items are matched against the exclude list here, once, on the plugin's thread, not by the
    // engine thread for every missing item of every player it dresses.
    if (next.items.size() > max_items) next.items.resize(max_items);
    if (next.exclude.size() > max_exclude) next.exclude.resize(max_exclude);
    for (auto &rule : next.exclude) rule = lower(rule);
    next.kept.assign(next.items.size(), true);
    for (std::size_t i = 0; i < next.items.size(); ++i) next.kept[i] = !excluded(next, next.items[i]);
    std::lock_guard lock(mutex);
    if (policy && policy->owner != next.owner) return "\"" + policy->owner + "\" chooses the stand-ins already";
    policy = std::make_shared<const Policy>(std::move(next));
    changes.fetch_add(1, std::memory_order_acq_rel);
    return {};
}
void clear(std::string_view owner) {
    std::lock_guard lock(mutex);
    if (!policy || policy->owner != owner) return;
    policy.reset();
    changes.fetch_add(1, std::memory_order_acq_rel);
}
} // namespace dingosdk::multiplayer::stand_ins
