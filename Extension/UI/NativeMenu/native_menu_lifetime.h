#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::multiplayer::menu_data {
// Native transitions also unload assets when no ReSkate load request exists.
// Suspend publication and queued actions before cleanup can dispatch callbacks.
// Only a new active world may resume them; shutdown remains permanent.
class MenuLifetime {
    enum class Phase { loading, active, shutdown };
    std::atomic<Phase> phase_{Phase::loading};
public:
    bool blocked() const noexcept { return phase_.load() != Phase::active; }
    template<class Cleanup> void before_transition(unsigned next, Cleanup&& cleanup) {
        if (next == 24) {
            if (phase_.exchange(Phase::shutdown) != Phase::shutdown) cleanup();
        } else if (next == 14 || next == 22 || next == 3) {
            auto expected = Phase::active;
            if (phase_.compare_exchange_strong(expected, Phase::loading)) cleanup();
        } else if (next == 13 || next == 21) {
            auto expected = Phase::loading;
            phase_.compare_exchange_strong(expected, Phase::active);
        }
    }
};

// The selected activity enum is shared by every native challenge. A custom
// Details request may close only the exact page it mounted, never a later
// stock page that happens to select the same enum or reuse an item id.
struct ActivityDetailsOwnership {
    std::uint64_t page{}, layer{};
    int item{};
    bool requested{};
    void request() noexcept { requested = true; }
    void mount(std::uint64_t next_page, std::uint64_t next_layer, int next_item) noexcept {
        page = next_page; layer = next_layer; item = next_item; requested = true;
    }
    bool owns(std::uint64_t live_page, std::uint64_t live_layer, int live_item) const noexcept {
        return requested && page && layer && live_page == page && live_layer == layer && live_item == item;
    }
    // Back can deactivate the item before teardown runs. Cached presenters
    // still retain this page and must be restored before a second entry.
    bool retains(std::uint64_t live_page) const noexcept {
        return requested && page && live_page == page;
    }
    void clear_mount() noexcept { page = layer = 0; item = 0; }
    void retire() noexcept { clear_mount(); requested = false; }
};

// Native navigation is asynchronous and its action slot can be busy. Keep
// ownership until Back is consumed; never abandon it on a failed enqueue.
class ActivityDetailsClose {
    std::uint64_t since_{};
    bool pending_{},queued_{};
public:
    void begin(std::uint64_t now) noexcept {
        if(!pending_) {since_=now;pending_=true;queued_=false;}
    }
    bool pending() const noexcept { return pending_; }
    bool needs_back() const noexcept { return pending_ && !queued_; }
    void back_queued(bool accepted) noexcept { if(accepted)queued_=true; }
    bool needs_selection_recovery(std::uint64_t now,bool exact_page,bool owned_selection) const noexcept {
        return pending_ && exact_page && owned_selection && now>=since_ && now-since_>=2000;
    }
    void reset() noexcept { *this={}; }
};

// The native Layer_Menu capture has TargetIndex=-1 after its transition has
// finished. StackIndex identifies the current item; TargetIndex describes a
// transition request and must never select a page for publication or teardown.
inline std::optional<unsigned> active_activity_stack_item(int stack_index, unsigned count, bool active) noexcept {
    if(!active || stack_index<0 || static_cast<unsigned>(stack_index)>=count)return std::nullopt;
    return static_cast<unsigned>(stack_index);
}

// Capture the actual normalized Start/Menu button before UI publication is
// throttled. Opening another phone page is not evidence of a Start press.
// Never reopen on a held key, a closing press, or a new match entered held.
class MatchPanelStartInput {
    bool down_{}, menu_open_{}, available_{}, initialized_{};
public:
    bool sample(std::uint32_t buttons, bool escape, bool available, bool menu_open,bool owned_panel=false) noexcept {
        const bool down=(buttons & 0x0010u)!=0 || escape;
        const bool request=initialized_ && available_ && available && (!menu_open_ || owned_panel) && down && !down_;
        down_=down;menu_open_=menu_open;available_=available;initialized_=true;
        return request;
    }
};

// A picker page can retire on the very Start press that opens the phone.
// Input belongs to the current match; only real world/manager teardown resets
// its latch and request. The request is still checked against the exact match.
inline void reset_match_panel_input(MatchPanelStartInput& latch,std::string& request,bool preserve_match_input) {
    if(preserve_match_input)return;
    latch={};request.clear();
}

// Model creation returns an independently owned root. Dropping the C++ handle
// does not destroy it. Track partial builds and old pause-menu generations too.
template<class Model> class OwnedMenuModels {
    std::vector<Model> models_;
public:
    bool empty() const { return models_.empty(); }
    std::size_t checkpoint() const noexcept { return models_.size(); }
    void track(Model value) {
        if (std::none_of(models_.begin(), models_.end(), [&](const auto& existing) {
                return existing.handle == value.handle && existing.type == value.type;
            })) models_.push_back(value);
    }
    // For an unmounted build: release only the roots created after its start.
    // The caller must detach any published references first. Pop only after a
    // successful destroy so an interrupted rollback can safely be retried.
    template<class Destroy> void rollback(std::size_t checkpoint, Destroy&& destroy) {
        if(checkpoint>models_.size())throw std::out_of_range("Native menu model checkpoint is no longer valid.");
        while(models_.size()>checkpoint) {
            destroy(models_.back());
            models_.pop_back();
        }
    }
    template<class Detach, class Destroy> void release(Detach&& detach, Destroy&& destroy) {
        if (empty()) return;
        detach();
        while (!models_.empty()) {
            destroy(models_.back());
            models_.pop_back(); // Keep unfinished cleanup available on failure.
        }
    }
    // Use only after the native manager itself has been replaced.
    void manager_replaced() { models_.clear(); }
};

// A command one of a page's native buttons runs: the pause-menu generation it was
// last handed out in, and the render pass that last asked for it.
struct MenuAction { std::uint64_t generation{}; std::string command, argument; std::uint64_t pass{}; };
inline bool waiting_card_action(std::string_view command) noexcept {
    return command=="flag-wait-start" || command=="flag-wait-leave";
}
inline bool action_generation_current(const MenuAction& action,std::uint64_t owner) noexcept {
    // Waiting actions must also pass the exact visible card/flag check when
    // dispatched. Picker and match-specific actions retain their old guard.
    return action.generation && (action.generation==owner || waiting_card_action(action.command));
}

// The slot a button gets for a command. A slot is a native callback whose
// descriptor the game borrows for the rest of the process, so a page has a
// fixed number of them and they are reused, never added to: a command keeps
// the slot it has, whichever generation of the menu gave it out (every map
// load starts a new one); a new command takes the next unused slot; and with
// none left, the slot asked for longest ago. Every button asks on every render
// pass, so a slot not asked for in this pass or the one before is on no button.
// Empty when every slot is on a button.
template<class At> std::optional<std::size_t> action_slot(std::size_t used, std::size_t capacity, At&& at,
        std::string_view command, std::string_view argument, std::uint64_t pass) {
    std::optional<std::size_t> idle;
    for (std::size_t i = 0; i < used; ++i) {
        const MenuAction& action = at(i);
        if (action.command == command && action.argument == argument) return i;
        if (action.pass + 1 < pass && (!idle || action.pass < at(*idle).pass)) idle = i;
    }
    return used < capacity ? used : idle;
}
} // namespace dingosdk::multiplayer::menu_data
