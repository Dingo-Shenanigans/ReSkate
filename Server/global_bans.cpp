#include "global_bans.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Extension/Multiplayer/word_lists.h"
#ifdef _WIN32
#include "Engine/Vfs/https_download.h"
#else
#include <cstdio>
#endif
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>

namespace dingosdk::server {
namespace {
constexpr std::size_t max_bytes = 256 * 1024;

#ifdef _WIN32
std::string download() {
    const auto url = multiplayer::identity_lists_url;
    https::Download result;
    auto answer = https::get_text(std::wstring(url.begin(), url.end()), max_bytes, 15, L"ReSkateServer/1", &result);
    if (!answer) throw std::runtime_error("HTTP " + std::to_string(result.http_status) + ", error " + std::to_string(result.error));
    return std::move(*answer);
}
#else
// curl does the TLS: the Linux server has no HTTPS client of its own (server_update.cpp).
// The command is fixed text, with nothing from a player or the config in it.
std::string download() {
    const auto command = "curl --silent --show-error --fail --max-time 15 --max-filesize " + std::to_string(max_bytes) +
                         " --proto =https --user-agent ReSkateServer/1 " + std::string(multiplayer::identity_lists_url) + " 2>&1";
    auto *pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("curl could not be started");
    std::string output;
    char buffer[4096];
    for (std::size_t count; (count = fread(buffer, 1, sizeof(buffer), pipe)) > 0;)
        if (output.size() < max_bytes) output.append(buffer, count);
    if (pclose(pipe) != 0) {
        // curl's own words (or the shell's, when curl is not installed), on one line.
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        std::replace(output.begin(), output.end(), '\n', ' ');
        throw std::runtime_error(output.empty() ? "curl failed" : output);
    }
    return output;
}
#endif
} // namespace

BanListCheck use_ban_list(std::string_view answer) {
    static std::optional<std::vector<std::uint64_t>> last;
    BanListCheck check;
    try {
        auto lists = multiplayer::parse_identity_lists(answer);
        // (Read before anything is put in use: an answer that is wrong anywhere changes nothing.)
        const auto words = multiplayer::use_word_lists(answer);
        multiplayer::server_tokens_rule = multiplayer::parse_server_tokens_required(answer);
        const auto &bans = lists[static_cast<std::size_t>(multiplayer::IdentityList::banned)];
        check.banned = bans.size();
        check.changed = last != bans;
        last = bans;
        multiplayer::publish_identity_lists(std::move(lists));
        check.words_changed = words.changed;
        check.filtered_words = words.filtered;
        check.forbidden_words = words.forbidden;
        check.ok = true;
    } catch (const std::exception &e) {
        check.problem = std::string("the answer was not the lists: ") + e.what();
    }
    return check;
}
BanListCheck read_global_bans() {
    try {
        return use_ban_list(download());
    } catch (const std::exception &e) {
        BanListCheck check;
        check.problem = e.what();
        return check;
    }
}
} // namespace dingosdk::server
