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
    std::lock_guard lock(mutex);
    if (policy && policy->owner != next.owner) return "\"" + policy->owner + "\" chooses the stand-ins already";
    if (next.items.size() > max_items) next.items.resize(max_items);
    if (next.exclude.size() > max_items) next.exclude.resize(max_items);
    for (auto &rule : next.exclude) rule = lower(rule);
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
