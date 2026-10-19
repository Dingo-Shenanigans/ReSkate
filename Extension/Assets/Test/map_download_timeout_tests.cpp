#include "Extension/Assets/map_download.h"
#include "Extension/Assets/live_mods.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Launcher/mod_manager.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fixture {
namespace fs = std::filesystem;
namespace policy = dingosdk::multiplayer::map_fetch_policy;
fs::path root;
std::atomic<std::uint64_t> clock{1000000};
constexpr auto use_download_deadline = static_cast<std::uint64_t>(-1);
std::atomic<std::uint64_t> active_deadline{use_download_deadline};
std::atomic<bool> downloading{}, installing{}, release_download{}, release_install{}, merging{};
std::atomic<bool> fetching{true}, held{}, observed_cancel{}, installed_cancel{};
std::atomic<unsigned> downloads{}, installs{}, applies{}, stops{}, rejoins{};
std::atomic<std::uint64_t> queued_stop_deadline{};
bool block_download{}, block_install{}, slow{}, transfer_error{}, fallback{};
dingosdk::MultiplayerModel session;
std::mutex messages_mutex, gate_mutex;
std::condition_variable gate;
std::vector<std::string> notices;
std::vector<std::uint64_t> http_budgets;
fs::path archive;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate>
void wait(Predicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
bool noticed(std::string_view text) {
    std::lock_guard lock(messages_mutex);
    for (const auto& notice : notices)
        if (notice.find(text) != std::string::npos) return true;
    return false;
}
}

namespace dingosdk::multiplayer {
std::uint64_t fake_now_us() noexcept { return fixture::clock.load(); }
}

// Compile the actual lifecycle, not a parallel model. Preincluded headers keep these
// substitutions local to this translation unit; archives also stay in the test fixture.
DWORD test_get_temp_path(DWORD capacity, LPWSTR output) {
    const auto path = fixture::root.wstring() + L"\\";
    if (path.size() >= capacity) return 0;
    std::copy(path.begin(), path.end(), output);
    output[path.size()] = L'\0';
    return static_cast<DWORD>(path.size());
}
#define now_us fake_now_us
#define GetTempPathW test_get_temp_path
#include "../map_download.cpp"
#undef GetTempPathW
#undef now_us

namespace dingosdk::https {
std::optional<std::string> get_text(std::wstring_view, std::uint64_t, std::uint32_t,
    const wchar_t*, Download* result, Watch* watch) {
    fixture::require(watch && watch->deadline_us.load() == map_download::deadline.load(),
        "metadata shares the session deadline");
    if (result) *result = {true, 200, 0};
    return R"({"namespace":"Team","name":"Map","community_listings":[{"community":"reskate","categories":["Maps"]}],
        "latest":{"version_number":"2.0.0","description":"Test map"}})";
}
Download get(std::wstring_view, const std::filesystem::path& destination,
    std::uint64_t max_bytes, std::uint32_t timeout_seconds, const wchar_t*, Watch* watch) {
    using namespace fixture;
    require(max_bytes == (8ull << 30), "size restriction retained");
    require(timeout_seconds == 4 * 60 * 60, "HTTP keeps its own four hour limit, not the session's");
    archive = destination;
    const auto request = ++downloads;
    http_budgets.push_back(watch->deadline_us.load());
    std::ofstream(destination, std::ios::binary) << "partial archive";
    watch->received.store(10);
    watch->total.store(100);
    downloading.store(true);
    if (block_download) {
        std::unique_lock lock(gate_mutex);
        gate.wait(lock, [] { return release_download.load(); });
    }
    if (slow) {
        // Real download progress still cannot extend either absolute deadline.
        fixture::clock.fetch_add(10 * 60 * policy::second_us);
        watch->received.store(90);
    }
    observed_cancel.store(watch->cancel.load());
    if (watch->cancel.load()) return {false, 0, ERROR_CANCELLED};
    if (fallback && request == 1) {
        fixture::clock.fetch_add(60 * policy::second_us);
        return {false, 404, 0};
    }
    if (transfer_error) return {false, 0, ERROR_WINHTTP_TIMEOUT};
    watch->received.store(100);
    return {true, 200, 0};
}
}

namespace dingosdk::mods {
std::filesystem::path engine_data_root() noexcept { return fixture::root; }
ModList scan_mods(const std::filesystem::path&) noexcept { return {}; }
void save_mod_order(const std::filesystem::path&, const std::vector<ModEntry>&) {}
}
namespace dingosdk::launcher_mods {
std::string install(const std::filesystem::path& root, const std::filesystem::path& source, bool,
    const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options) {
    using namespace fixture;
    require(fs::exists(source), "installer receives completed archive");
    ++installs;
    installing.store(true);
    if (block_install) {
        std::unique_lock lock(gate_mutex);
        gate.wait(lock, [] { return release_install.load(); });
    }
    progress(0.5f);
    installed_cancel.store(cancel.load());
    if (cancel.load()) throw std::runtime_error("cancelled");
    fs::create_directories(root / options.folder);
    std::ofstream(root / options.folder / "reskate-levels.json") << "{}";
    return options.folder;
}
void remove(const std::filesystem::path&, const std::string&) {}
}
namespace dingosdk::live_mods {
bool only_maps(const std::filesystem::path&) { return true; }
bool busy() noexcept { return fixture::merging.load(); }
std::string apply(bool reload) {
    fixture::require(!reload, "map apply must not reload prematurely");
    ++fixture::applies;
    fixture::merging.store(true);
    return "applying";
}
Progress progress() { return {1, 2, "map"}; }
std::vector<std::string> applied_mods() { return {"Team-Map"}; }
std::string status() { return "applied"; }
}
namespace dingosdk::multiplayer {
MultiplayerModel model() { return fixture::session; }
bool fetching_map() noexcept { return fixture::fetching.load(); }
std::uint64_t map_fetch_deadline() noexcept {
    const auto active = fixture::active_deadline.load();
    return active == fixture::use_download_deadline ? map_download::deadline.load() : active;
}
void hold_map_load(bool hold, std::string_view package) noexcept {
    fixture::held.store(hold);
    if (hold && package != "Team-Map-1.0.0") std::abort();
}
bool queue_command(std::string_view action, std::string_view argument, std::string_view password) {
    if (action == "stop") ++fixture::stops;
    if (action == "stop-map-fetch") {
        const auto expected = map_download::deadline.load();
        fixture::require(expected && argument == std::to_string(expected) && password.empty(),
            "map stop carries the captured originating attempt deadline");
        fixture::queued_stop_deadline.store(expected);
        ++fixture::stops;
    }
    if (action == "rejoin") ++fixture::rejoins;
    return true;
}
std::string map_package_name(std::string_view folder, std::string_view version) {
    return std::string(folder) + "-" + std::string(version);
}
}
namespace dingosdk::overlay {
void set_map_download_icon(const std::string&) noexcept {}
void notify(NoticeLevel, std::string title, std::string text) noexcept {
    std::lock_guard lock(fixture::messages_mutex);
    fixture::notices.push_back(title + ": " + text);
}
}
namespace dingosdk::logging {
bool enabled(Level) noexcept { return false; }
void write(Level, Context, Channel, std::string_view) noexcept {}
void write(Level, Channel, std::string_view) noexcept {}
}

namespace fixture {
namespace download = dingosdk::map_download;
using Stage = download::Stage;

void drain() {
    release_download.store(true);
    release_install.store(true);
    gate.notify_all();
    wait([] { return download::workers.load() == 0; }, "workers did not drain");
}
void reset() {
    drain();
    require(download::view().stage == Stage::idle, "previous lifecycle did not finish");
    fs::remove_all(root / "Mods");
    clock.store(1000000);
    active_deadline.store(use_download_deadline);
    downloading.store(false);
    installing.store(false);
    release_download.store(false);
    release_install.store(false);
    merging.store(false);
    fetching.store(true);
    held.store(false);
    observed_cancel.store(false);
    installed_cancel.store(false);
    downloads.store(0);
    installs.store(0);
    applies.store(0);
    stops.store(0);
    queued_stop_deadline.store(0);
    rejoins.store(0);
    block_download = block_install = slow = transfer_error = fallback = false;
    session = {};
    session.active = true;
    session.connected = false;
    notices.clear();
    http_budgets.clear();
    archive.clear();
    download::last_failed.clear();
}
void offer(std::uint64_t deadline = 0, bool update = false) {
    dingosdk::multiplayer::MapNeed need{"Team-Map-1.0.0", "Test map", true, false,
        update ? "0.9.0" : "", deadline};
    download::offer(need);
    wait([] { return download::asking(); }, "metadata did not reach confirmation");
    wait([] { return download::workers.load() == 0; }, "metadata worker did not drain");
}
void start_apply() {
    wait([] { return download::view().stage == Stage::applying; }, "did not install and reach applying");
    drain();
    download::tick();
    require(applies.load() == 1 && held.load(), "apply starts once and holds session loading");
}
void applying() {
    download::answer(true);
    start_apply();
}
void joined() {
    merging.store(false);
    download::tick();
    require(download::view().stage == Stage::joining, "completed merge advances to joining");
    fetching.store(false);
    session.connected = true;
    clock.fetch_add(2500000);
    download::tick();
    require(download::view().stage == Stage::idle, "joined session clears lifecycle");
    require(!held.load() && stops.load() == 0 && rejoins.load() == 0, "normal join does not leave or rejoin");
}
void idle(const char* reason, unsigned expected_stops = 1) {
    require(download::view().stage == Stage::idle, reason);
    drain();
    require(stops.load() == expected_stops, "failure only stops its originating waiting session");
    if (expected_stops)
        require(queued_stop_deadline.load() == download::deadline.load(),
            "queued map stop identifies the originating attempt for dispatch-time ownership validation");
    download::tick();
    require(held.load() == (merging.load() && download::apply_started.load()),
        "failure holds map loading only while an uncancellable live merge drains");
    require(archive.empty() || !fs::exists(archive), "partial archive removed");
}

void tests() {
    reset();
    offer();
    applying();
    require(downloads.load() == 1 && installs.load() == 1, "success downloads and installs exactly once");
    joined();

    reset();
    merging.store(true);
    offer();
    download::answer(true);
    wait([] { return download::view().stage == Stage::applying; }, "unrelated live merge blocked map offer");
    drain();
    download::tick();
    require(applies.load() == 0 && held.load(), "map apply waits for unrelated live merge without rejecting offer");
    merging.store(false);
    download::tick();
    require(applies.load() == 1, "map apply starts after unrelated merge drains");
    joined();

    reset();
    block_download = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "download did not start for duplicate confirmation");
    clock.fetch_add(policy::second_us);
    download::answer(true);
    require(downloads.load() == 1 && download::workers.load() == 1, "only winning confirmation claims a worker");
    release_download.store(true);
    gate.notify_all();
    start_apply();
    joined();

    reset();
    block_download = true;
    slow = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "slow download did not start");
    clock.fetch_add(10 * 60 * policy::second_us);
    download::watch.received.store(50);
    download::tick();
    require(download::view().stage == Stage::downloading && download::view().received == 50 && held.load(),
        "progressing transfer remains active beyond old five minute deadline");
    release_download.store(true);
    gate.notify_all();
    start_apply();
    require(clock.load() > 5 * 60 * policy::second_us, "slow progressing transfer exceeds former five minute limit");
    joined();

    reset();
    transfer_error = true;
    offer();
    download::answer(true);
    wait([] { return download::view().stage == Stage::idle; }, "HTTP timeout did not fail");
    idle("transfer timeout");
    require(installs.load() == 0 && noticed("timed out"), "HTTP timeout cannot install");
    transfer_error = false;
    stops.store(0);
    offer();
    applying();
    require(downloads.load() == 2, "timeout allows same package retry");
    joined();

    // The session stops waiting (its deadline passes and it ends itself) while the map downloads:
    // the download is not thrown away, the map installs and is applied, and the player is told to
    // join again (the session would not take them back past its deadline).
    reset();
    block_download = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "download did not start");
    clock.store(download::deadline.load());
    session.active = false;
    fetching.store(false);
    active_deadline.store(0);
    download::tick();
    require(download::view().stage == Stage::downloading && !download::watch.cancel.load() && noticed("still downloading"),
        "session expiry leaves an active download running and says so");
    release_download.store(true);
    gate.notify_all();
    start_apply();
    require(!observed_cancel.load() && installs.load() == 1, "download that outlived its session installs");
    merging.store(false);
    download::tick();
    require(download::view().stage == Stage::joining, "detached download is applied");
    clock.fetch_add(2500000);
    download::tick();
    require(download::view().stage == Stage::idle && rejoins.load() == 0 && stops.load() == 0 && noticed("Map installed"),
        "detached download ends installed, without rejoining an expired session");

    reset();
    block_install = true;
    offer();
    download::answer(true);
    wait([] { return installing.load(); }, "install did not start");
    clock.store(download::deadline.load());
    download::tick();
    drain();
    require(!installed_cancel.load() && installs.load() == 1, "session expiry does not cancel an install under way");
    wait([] { return download::view().stage == Stage::applying; }, "expired install did not go on to applying");
    download::tick();
    require(applies.load() == 1, "expired install is still applied");
    merging.store(false);
    download::tick();
    fetching.store(false);
    session.active = false;
    clock.fetch_add(2500000);
    download::tick();
    require(download::view().stage == Stage::idle && rejoins.load() == 0 && noticed("Map installed"),
        "map installed after the session's deadline is not rejoined");

    // A live merge cannot be stopped: a fresh need waits for one left behind by a cancelled attempt.
    reset();
    offer();
    applying();
    active_deadline.store(download::deadline.load() + 1); // another session's own attempt
    download::tick();
    idle("apply cancelled by another session", 0);
    require(merging.load() && rejoins.load() == 0, "cancelled atomic merge must not join");
    download::offer({"Team-Map-1.0.0", "Test map", true});
    require(download::view().stage == Stage::idle && noticed("still finishing"), "busy live merge blocks retry");
    merging.store(false);
    download::tick();
    require(rejoins.load() == 0 && !held.load(), "finished cancelled apply cannot rejoin and releases load hold");

    reset();
    offer();
    download::answer(false);
    idle("decline");
    require(downloads.load() == 0 && noticed("cancelled"), "decline never downloads");

    reset();
    offer();
    clock.store(download::ask_until.load() - 1);
    download::tick();
    require(download::asking(), "confirmation remains open one microsecond before deadline");
    clock.fetch_add(1);
    download::answer(true);
    idle("confirmation exact boundary");
    require(downloads.load() == 0 && noticed("two minutes"), "late click cannot bypass confirmation timeout");

    reset();
    offer();
    clock.store(download::ask_until.load());
    download::tick();
    idle("confirmation tick timeout");
    require(downloads.load() == 0 && noticed("two minutes"), "tick expires unanswered prompt");

    reset();
    const auto short_session = clock.load() + policy::confirmation_us / 2;
    offer(short_session);
    require(download::ask_until.load() == short_session, "confirmation is capped by existing session deadline");
    clock.store(short_session);
    download::answer(true);
    idle("shorter session confirmation boundary");
    require(downloads.load() == 0 && noticed("timed out"), "confirmation never extends existing session budget");

    reset();
    offer(clock.load() + 1);
    download::answer(true);
    wait([] { return download::view().stage == Stage::applying; }, "download confirmed in the session's last microsecond did not start");
    require(downloads.load() == 1 && http_budgets[0] == 0, "a download confirmed in time starts, with no session deadline on it");
    drain();
    download::tick();
    merging.store(false);
    download::tick();
    fetching.store(false);
    session.active = false;
    clock.fetch_add(2500000);
    download::tick();
    require(download::view().stage == Stage::idle && noticed("Map installed"), "late download still ends installed");

    reset();
    block_download = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "HTTP did not start for cancel race");
    download::answer(false);
    require(download::watch.cancel.load(), "cancel flag is immediate");
    download::tick();
    require(held.load(), "cancelled HTTP keeps map loading held until worker drains");
    download::offer({"Team-Map-1.0.0", "Test map", true});
    require(downloads.load() == 1 && noticed("still finishing"), "retry cannot reuse a draining HTTP worker");
    drain();
    require(observed_cancel.load() && !fs::exists(archive) && installs.load() == 0, "cancel cleans archive without install");
    // The same package is retriable after a user cancellation.
    block_download = false;
    offer();
    applying();
    require(downloads.load() == 2, "retry fetches same package again");
    // The original user cancellation stopped its waiting attempt.
    stops.store(0);
    joined();

    reset();
    fallback = true;
    offer();
    applying();
    require(downloads.load() == 2 && http_budgets.size() == 2, "404 selects newest fallback");
    require(http_budgets[0] == 0 && http_budgets[1] == 0, "neither request is held to the session's deadline");
    joined();

    reset();
    fallback = true;
    offer(0, true);
    download::answer(true);
    wait([] { return download::view().stage == Stage::idle; }, "missing host version did not fail");
    idle("host version consistency");
    require(downloads.load() == 1 && installs.load() == 0, "version mismatch never falls back to another host version");

    reset();
    offer();
    const auto original_deadline = download::deadline.load();
    download::offer({"Team-Other-1.0.0", "Other map", true});
    require(download::asking() && download::deadline.load() == original_deadline && stops.load() == 1,
        "host changing maps does not replace in-flight request or extend its deadline");
    stops.store(0);
    download::answer(false);
    idle("host map change");

    reset();
    block_download = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "download did not start for session replacement");
    active_deadline.store(0);
    download::tick();
    idle("session identity changed", 0);
    require(observed_cancel.load() && installs.load() == 0,
        "download cancels when the active session no longer owns its deadline");

    reset();
    offer();
    active_deadline.store(0);
    fetching.store(false);
    session.connected = true;
    download::tick();
    require(download::view().stage == Stage::idle && !held.load() && downloads.load() == 0 &&
            stops.load() == 0 && rejoins.load() == 0,
        "abandoned confirmation cancels without holding or stopping a fresh unrelated ordinary join");
    download::answer(true);
    require(downloads.load() == 0, "late confirmation cannot start download for abandoned session");

    reset();
    offer();
    const auto abandoned_deadline = download::deadline.load();
    clock.fetch_add(policy::second_us);
    const auto fresh_deadline = clock.load() + policy::session_us;
    active_deadline.store(fresh_deadline);
    download::offer({"Team-Map-1.0.0", "Test map", true, false, "", fresh_deadline});
    wait([] { return download::asking(); }, "fresh same-package need was not retaken");
    drain();
    require(download::deadline.load() == fresh_deadline && fresh_deadline != abandoned_deadline && stops.load() == 0,
        "fresh same-package attempt replaces stale confirmation without stopping its new session");
    applying();
    joined();

    reset();
    block_download = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "old download did not start for queued replacement");
    clock.fetch_add(policy::second_us);
    const auto queued_deadline = clock.load() + policy::session_us;
    active_deadline.store(queued_deadline);
    download::offer({"Team-Map-1.0.0", "Test map", true, false, "", queued_deadline});
    download::tick();
    require(download::view().stage == Stage::idle && download::watch.cancel.load() &&
            downloads.load() == 1 && stops.load() == 0 && download::pending &&
            download::pending->deadline_us == queued_deadline,
        "fresh offer queues while cancelled worker drains without stopping replacement session");
    drain();
    require(observed_cancel.load() && installs.load() == 0 && !fs::exists(archive),
        "old worker cleans partial archive before queued replacement can use its globals");
    block_download = false;
    download::tick();
    wait([] { return download::asking(); }, "queued same-package need was not offered after drain");
    drain();
    require(download::deadline.load() == queued_deadline && !download::pending && stops.load() == 0,
        "queued attempt keeps its fresh deadline");
    applying();
    require(downloads.load() == 2, "queued fresh attempt starts exactly one new transfer");
    joined();

    reset();
    block_download = true;
    offer();
    download::answer(true);
    wait([] { return downloading.load(); }, "old download did not start for stale pending need");
    clock.fetch_add(policy::second_us);
    const auto superseded_deadline = clock.load() + policy::session_us;
    active_deadline.store(superseded_deadline);
    download::offer({"Team-Map-1.0.0", "Test map", true, false, "", superseded_deadline});
    active_deadline.store(0);
    drain();
    download::tick();
    require(download::view().stage == Stage::idle && !download::pending && downloads.load() == 1 &&
            stops.load() == 0 && !held.load(),
        "queued need is discarded when its replacement session also ends before worker drain");

    reset();
    offer();
    applying();
    merging.store(false);
    download::tick();
    clock.store(download::join_at.load() + policy::installed_load_us - 1);
    download::tick();
    require(download::view().stage == Stage::joining, "installed map load retains its last microsecond");
    clock.fetch_add(1);
    download::tick();
    idle("installed map load timeout");
    require(noticed("still cannot load"), "installed map reports load failure at 30 seconds");

    reset();
    offer();
    applying();
    merging.store(false);
    download::tick();
    fetching.store(false);
    session.active = false;
    clock.fetch_add(2500000);
    download::tick();
    require(download::view().stage == Stage::idle && rejoins.load() == 1 && stops.load() == 0,
        "successful install rejoins when the original session ended independently");

    reset();
    offer();
    applying();
    clock.store(download::deadline.load() - 2);
    merging.store(false);
    download::tick();
    require(download::view().stage == Stage::joining, "near-deadline apply reaches joining");
    clock.fetch_add(1);
    session.connected = true;
    fetching.store(false);
    active_deadline.store(0);
    clock.fetch_add(2);
    download::tick();
    require(download::view().stage == Stage::idle && !held.load() && stops.load() == 0 &&
            rejoins.load() == 0 && !noticed("timed out"),
        "join completed within session budget retires successfully even when next tick is after deadline");

    reset();
    download::tick();
    require(!held.load() && downloads.load() == 0 && stops.load() == 0 && rejoins.load() == 0,
        "ordinary join without download is untouched");
}
}

int main(int argc, char** argv) {
    try {
        fixture::require(argc == 2, "pass a fixture directory");
        fixture::root = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(fixture::root);
        fixture::tests();
        fixture::drain();
        std::filesystem::remove_all(fixture::root);
        std::cout << "Production map download timeout lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        // A bounded wait failure can leave detached production workers alive. Do not
        // destroy their fixture globals while they may still be using them.
        std::_Exit(1);
    }
}
