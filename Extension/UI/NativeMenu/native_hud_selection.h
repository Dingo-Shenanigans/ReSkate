#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <span>

namespace dingosdk::multiplayer::menu_data {
// Compiled native StackScreenController.ShowStackItemWithContent. Completion
// runs before rendering; only this exact ABI may prepare a pending Page body.
inline bool native_activity_page_publish_graph(std::uint32_t hash,const std::array<std::uint32_t,10>& layout) noexcept {
    constexpr std::array<std::uint32_t,10> expected{112,288,1574,287,15,0,0,983055,2162688,16978432};
    return hash==0x64c856aa && layout==expected;
}
struct HudRootCandidate {
    std::uint64_t handle{};
    bool active{};
    unsigned gameplay_stacks{};
};
// An inactive parent is a native transition, not a hidden custom item. Its
// bindings can already be retired; do not repair children until it resumes.
inline bool hud_publication_allowed(bool parent_active,bool menu_open,bool panel_owned) noexcept {
    // An unconfirmed request leaves gameplay visible. Actual native menu or
    // parent inactivity still prevents unsafe publication during transitions.
    return parent_active && !menu_open && !panel_owned;
}
// A cached active Page is not proof that the native menu owns gameplay.
// Confirm its menu state and camera focus continuously before borrowing it.
class ActivityMenuTransitionGate {
    std::uint64_t page_{},since_{};
    bool observed_{};
public:
    bool ready(std::uint64_t page_handle,bool menu_open,bool selection_owned,bool camera_focused,std::uint64_t now) noexcept {
        if(!page_handle || !menu_open || !selection_owned || !camera_focused) {
            page_=0;observed_=false;return false;
        }
        if(!observed_ || page_!=page_handle) {page_=page_handle;since_=now;observed_=true;}
        return now>=since_ && now-since_>=150;
    }
};
// Wait briefly for a valid native parent on entry or after a real transition.
// Custom HUD curves are opaque, so no stock fade duration needs to elapse.
class HudPublicationGate {
    std::uint64_t root_{},generation_{},since_{};
    bool waiting_{true},observed_{};
public:
    bool allow(std::uint64_t root,std::uint64_t generation,bool ready,std::uint64_t now) noexcept {
        if(root_!=root || generation_!=generation) {root_=root;generation_=generation;observed_=false;waiting_=true;}
        if(!root || !ready) {observed_=false;waiting_=true;return false;}
        if(!observed_) {since_=now;observed_=true;}
        if(waiting_ && now-since_<100)return false;
        waiting_=false;return true;
    }
};
// A retained native menu-layer item alone is not an open menu. Such cached
// selections must not dismiss a persistent gameplay shortcut.
inline bool hud_shortcut_visible(bool available,bool menu_open,bool panel_open,bool opening,bool quitting) noexcept {
    return available && !menu_open && !panel_open && !opening && !quitting;
}
class HudResultsWindow {
    std::uint64_t match_{},ended_{};
    bool finished_{};
public:
    bool visible(std::uint64_t match,bool finished,std::uint64_t now) noexcept {
        if(match_!=match || !finished){match_=match;ended_=now;finished_=false;}
        if(!match || !finished)return false;
        if(!finished_){ended_=now;finished_=true;}
        const auto age=now>=ended_?now-ended_:0;
        return age>=4000 && age<16000;
    }
};
struct HudCountdownVisibility { bool countdown{},intro{},go{}; };
// Timing belongs to the challenge phase, independently of optional artwork.
// A failed scoreboard build must not prevent the preceding GO from expiring.
class HudCountdownLifecycle {
    std::uint64_t game_{},go_until_{};
    bool was_counting_{};
public:
    HudCountdownVisibility update(std::uint64_t game,bool counting,bool playing,
            std::uint64_t remaining,std::uint64_t now) noexcept {
        if(game_!=game) {game_=game;go_until_=0;was_counting_=false;}
        if(!game) {go_until_=0;was_counting_=false;return {};}
        if(playing && was_counting_)go_until_=now+900;
        if(!counting && !playing)go_until_=0;
        was_counting_=counting;
        const bool intro=counting && remaining>3000;
        const bool go=playing && now<go_until_;
        return {(counting && !intro)||go,intro,go};
    }
};
struct NotificationMountNode {
    std::uint64_t model{};
    std::uint32_t type{};
    std::uint64_t content{};
};
struct NotificationMount {
    std::uint64_t presentation{}, model{};
    bool operator==(const NotificationMount&) const = default;
};
// Only a private model or a deliberately hidden slot belongs to our adapter.
// A new native notification in the same slot must keep its own presentation.
inline bool owns_waiting_presentation(std::uint64_t current,std::uint64_t custom,bool hidden) {
    return custom && (current==custom || (!current && hidden));
}
// The backend reuses a waiting card and its input delegates across challenges.
// Resolve actions through its visible private presentation and current flag.
inline bool owns_waiting_action(std::uint64_t requested_source,std::uint64_t source,
        std::uint64_t current,std::uint64_t custom,bool registration_owned,
        bool retiring,bool lobby) noexcept {
    return requested_source && requested_source==source && custom && current==custom &&
        registration_owned && !retiring && lobby;
}
// Native navigation can move our private notification into a new presenter.
// Recover its source independently of the previous presenter's identity so
// the next challenge can update its title and callbacks.
template<class Copies> std::uint64_t waiting_notification_source(
        std::uint64_t current,const Copies& copies) {
    for(const auto& [source,copy]:copies)if(current==copy.handle)return source;
    return current;
}
// The secondary HUD stack holds FoundationsNotificationViewModel wrappers,
// not NotificationViewModel directly. Resolve only mounted, known wrappers;
// scanning all registered notifications also includes unrendered defaults.
template<class Read> std::optional<NotificationMount> mounted_notification(
        std::uint64_t presentation, Read&& read) {
    for(unsigned depth=0; presentation && depth<8; ++depth) {
        const auto node=read(presentation);
        if(!node.model)return std::nullopt;
        if(node.type==0x1053b9ec)return NotificationMount{presentation,node.model};
        if(node.type!=0xffb83e48 || node.content==presentation)return std::nullopt;
        presentation=node.content;
    }
    return std::nullopt;
}
// Authored defaults are registered as roots too. They have no game-owned
// compass/scoring/notification items and are not connected to a HUD widget.
inline std::uint64_t mounted_hud_root(std::span<const HudRootCandidate> roots,
                                     std::uint64_t preferred = 0,
                                     std::uint64_t authored = 0) {
    // The native name alias identifies the authored default exactly. Once
    // excluded, a unique active instance is live even with every optional
    // stock HUD section hidden. Handles include generations, not type tags.
    if(authored) {
        std::uint64_t live{};unsigned count{};
        for(const auto& root:roots)if(root.handle && root.handle!=authored && root.active) {
            live=root.handle;++count;
        }
        if(count==1)return live;
    }
    std::uint64_t selected{};
    std::uint64_t retained{};
    unsigned most{};
    for (const auto& root : roots) {
        if (!root.handle || !root.active || root.handle==authored) continue;
        // Native score/compass stacks briefly empty during activity changes.
        // Retain only a previously verified live identity, never a new default.
        if (!root.gameplay_stacks) {
            if(root.handle==preferred)retained=root.handle;
            continue;
        }
        if (root.handle == preferred) return root.handle;
        if (root.gameplay_stacks > most) {
            selected = root.handle;
            most = root.gameplay_stacks;
        }
    }
    return selected?selected:retained;
}
} // namespace dingosdk::multiplayer::menu_data
