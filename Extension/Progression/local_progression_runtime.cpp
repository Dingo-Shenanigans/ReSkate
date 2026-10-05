#include "Extension/Profile/runtime_internal.h"
#include "local_challenge_runtime.h"
#include "local_neighborhood_runtime.h"
#include "local_rip_score_runtime.h"

namespace dingosdk {
using namespace profile_runtime;
// Public progression model and commands. Persistence owns all saved edits;
// existing native adapters hydrate them on their normal game-update paths.
ProgressionModel local_profile_progression() {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(s.native_mutex);
    if (s.progression_revision != s.store->revision()) {
        const auto saved_shared = s.store->shared_snapshot();
        const auto& saved = *saved_shared;
        ProgressionModel result;
        result.available = true;
        const auto owns = [&](std::string_view id) {
            const auto it = saved.entitlements.find(id);
            return it != saved.entitlements.end() && it->second;
        };
        for (const auto& stop : fixed_bus_stop_catalog())
            result.bus_stops.push_back({stop.number, stop.world,
                owns(stop.visibility_entitlement), owns(stop.collected_entitlement)});
        const auto policy = profile::challenge_policy(saved);
        result.challenges_enabled = policy.enabled;
        result.challenges_hidden = s.store->bool_option(profile::hide_challenges_option).value_or(false);
        for (const auto& [id, entry] : policy.catalog) {
            ChallengeProgressRow row{id, entry.type, entry.available, 0,
                profile::challenge_completed_criteria(saved, id), entry.goals.size(), entry.neighborhood};
            const auto& section = saved.extensions.at("challenges");
            if (section.contains("progress") && section.at("progress").contains(id))
                row.attempts = section.at("progress").at(id).at("attempt").get<std::uint64_t>();
            result.challenges.push_back(std::move(row));
        }
        const auto max = saved.bool_options.find(profile::max_neighborhood_ranks_option);
        result.ranks_maxed = max != saved.bool_options.end() && max->second;
        const auto everything = saved.bool_options.find(profile::unlock_everything_option);
        result.everything_unlocked = everything != saved.bool_options.end() && everything->second;
        constexpr std::array<const char*, 4> names{"Entertainment", "Financial", "Historic", "Stadium"};
        for (unsigned i = 0; i < profile::neighborhood_ids.size(); ++i) {
            const auto id = profile::neighborhood_ids[i];
            const auto rank = saved.neighborhood_ranks.find(id);
            result.districts[i] = {std::string(id), names[i], rank == saved.neighborhood_ranks.end() ? 0 : rank->second};
        }
        if (const auto score = profile::rip_score(saved)) {
            result.score_available = true;
            result.score = score->value; result.score_cap = score->cap; result.score_level = score->level;
        }
        s.progression_cache = std::move(result);
        s.progression_revision = saved.revision;
    }
    auto result = s.progression_cache;
    result.feedback = s.progression_feedback;
    return result;
}

bool set_local_progression(const std::vector<std::string>& args) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(s.native_mutex);
    try {
        const auto number = [&](unsigned index, std::uint64_t maximum) {
            if (index >= args.size()) throw std::runtime_error("Missing progression value.");
            const auto& text = args[index]; std::uint64_t value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > maximum)
                throw std::runtime_error("Invalid progression value.");
            return value;
        };
        if (args.size() == 3 && args[0] == "busstop") {
            s.store->set_bus_stop_state(static_cast<unsigned>(number(1, UINT8_MAX)), static_cast<unsigned>(number(2, 2)));
            s.progression_feedback = "Bus stop saved. Reload the level to refresh its icon and interaction.";
        } else if (args.size() == 2 && args[0] == "challenges") {
            const bool hidden = number(1, 1) == 0;
            s.store->set_bool_option(profile::hide_challenges_option, hidden);
            hidden_challenges().store(hidden, std::memory_order_release);
            challenge_runtime().next_catalog_poll = 0;
            s.progression_feedback = hidden ? "Challenges hidden. Saved progress is kept." : "Challenges shown.";
        } else if (args.size() == 4 && args[0] == "ripscore") {
            s.store->save_rip_score({static_cast<std::int64_t>(number(1, 1000000000)),
                static_cast<std::int64_t>(number(2, 1000000000)), static_cast<std::int32_t>(number(3, 10000))});
            rip_score_runtime().next_poll = 0;
            s.progression_feedback = "RIP score saved.";
        } else if (args.size() == 3 && args[0] == "district") {
            s.store->save_district_rank(args[1], static_cast<std::uint32_t>(number(2, 10000)));
            neighborhood_runtime().next_poll = 0;
            s.progression_feedback = "District level saved. Automatic max ranks is off.";
        } else if (args.size() == 2 && args[0] == "maxranks") {
            s.store->set_bool_option(profile::max_neighborhood_ranks_option, number(1, 1) != 0);
            neighborhood_runtime().next_poll = 0;
            s.progression_feedback = "District rank preference saved.";
        } else if (args.size() == 2 && args[0] == "unlockall") {
            // Read when the game builds its cosmetics catalogue, so the change
            // lands at the next start: on seeds every item, off hands the
            // unearned ones back through the existing reconcile.
            s.store->set_bool_option(profile::unlock_everything_option, number(1, 1) != 0);
            s.progression_feedback = "Unlock preference saved. Restart the game to apply it.";
        } else throw std::runtime_error("Unknown progression command.");
        return true;
    } catch (const std::exception& error) {
        s.progression_feedback = std::string("Not saved: ") + error.what();
        return false;
    }
}

}