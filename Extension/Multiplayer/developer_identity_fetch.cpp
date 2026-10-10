#include "developer_identity.h"
#include "word_lists.h"
#include "Engine/Core/Log/logging.h"
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
        const auto answer = https::get_text(std::wstring(identity_lists_url.begin(), identity_lists_url.end()), max_bytes, 10,
                                            L"ReSkate-Identity/1", &download);
        if (answer) {
            auto lists = parse_identity_lists(*answer);
            server_tokens_rule = parse_server_tokens_required(*answer);
            const auto developers = lists[0].size(), homies = lists[1].size(), creators = lists[2].size(),
                       bans = lists[static_cast<std::size_t>(IdentityList::banned)].size();
            if (publish_identity_lists(std::move(lists)) || failing)
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Identity lists: {} developer(s), {} homie(s), {} content creator(s), {} banned.", developers,
                             homies, creators, bans);
            // The chat word lists come in the same answer (word_lists.h).
            if (const auto words = use_word_lists(*answer); words.changed)
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Word lists: {} filtered and {} not allowed at all, from the ReSkate backend.", words.filtered,
                             words.forbidden);
            ok = true;
        } else if (!failing) {
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Identity lists unavailable (HTTP {}, error {}); trying again every minute.", download.http_status,
                         download.error);
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
