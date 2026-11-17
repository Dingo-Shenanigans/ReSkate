#include "developer_identity.h"
#include "word_lists.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Debug/trace.h"
#include "Engine/Vfs/https_download.h"
#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>

// The game's way of reading the lists: in the background, driven by the client tick.
namespace dingosdk::multiplayer {
namespace {
constexpr std::uint64_t max_bytes = 256 * 1024, refresh_ms = 10 * 60 * 1000, retry_ms = 60 * 1000;
std::atomic<bool> fetching{};
std::atomic<std::uint64_t> next_fetch{}; // GetTickCount64 time

void fetch() noexcept {
    // The log says when the lists arrive, change or stop arriving, not every refresh.
    static bool failing{};
    bool ok{};
    try {
        https::Download download;
        trace::Scope timing("backend.identity_lists", "http.client");
        const auto get = [&](std::string_view url) {
            return https::get_text(std::wstring(url.begin(), url.end()), max_bytes, 10, L"ReSkate-Identity/1", &download);
        };
        auto answer = get(identity_lists_url);
        // The backend out of reach: its copy on GitHub (developer_identity.h).
        const auto backend = download;
        if (!answer) answer = get(identity_lists_mirror_url);
        if (!answer) timing.fail();
        if (answer) {
            auto lists = parse_identity_lists(*answer);
            server_tokens_rule = parse_server_tokens_required(*answer);
            const auto count = [&](IdentityList list) { return lists[static_cast<std::size_t>(list)].size(); };
            const auto developers = count(IdentityList::developer), staff = count(IdentityList::staff), centrix = count(IdentityList::centrix),
                       homies = count(IdentityList::homie), creators = count(IdentityList::content_creator), bans = count(IdentityList::banned);
            if (publish_identity_lists(std::move(lists)) || failing)
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Identity lists: {} developer(s), {} staff, {} Centrix, {} homie(s), {} content creator(s), {} banned.", developers,
                             staff, centrix, homies, creators, bans);
            // The chat word lists come in the same answer (word_lists.h).
            if (const auto words = use_word_lists(*answer); words.changed)
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Word lists: {} filtered and {} not allowed at all, from the ReSkate backend.", words.filtered,
                             words.forbidden);
            ok = true;
        } else if (!failing) {
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Identity lists unavailable (backend: HTTP {}, error {}; GitHub copy: HTTP {}, error {}); trying again every minute.",
                         backend.http_status, backend.error, download.http_status, download.error);
        }
    } catch (const std::exception &e) {
        if (!failing)
            logging::log(logging::Level::warning, logging::Channel::runtime,
                         "Identity lists are invalid ({}); trying again every minute.", e.what());
    } catch (...) {}
    failing = !ok;
    // A failure keeps the lists already in use.
    next_fetch = GetTickCount64() + (ok ? refresh_ms : retry_ms);
    fetching = false;
}
} // namespace

void refresh_identity_lists() noexcept {
    if (GetTickCount64() < next_fetch || fetching.exchange(true)) return;
    try {
        std::thread(fetch).detach();
    } catch (...) {
        next_fetch = GetTickCount64() + retry_ms;
        fetching = false;
    }
}
} // namespace dingosdk::multiplayer
