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
    // Plain names go to the set, rules with a "*" stay in the list; at most the limit of each.
    std::vector<std::string> rules;
    for (auto &rule : next.exclude) {
        auto lowered = ascii_lower(rule);
        if (lowered.find('*') != std::string::npos) {
            if (rules.size() < max_exclude) rules.push_back(std::move(lowered));
        } else if (next.exclude_names.size() < max_exclude_names) {
            next.exclude_names.insert(std::move(lowered));
        }
    }
    next.exclude = std::move(rules);
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
