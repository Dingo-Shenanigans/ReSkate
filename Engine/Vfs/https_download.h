#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dingosdk::https {
struct Download {
    bool ok{};
    unsigned long http_status{}, error{};
};
// A long download as another thread sees it: how far it is (`total` 0 when the server does
// not say), and a way to stop it (the download then fails with ERROR_CANCELLED).
struct Watch {
    std::atomic<std::uint64_t> received{}, total{};
    std::atomic<bool> cancel{};
};
// Streams an HTTPS GET into `destination`, which must not exist yet. Follows
// redirects that stay on HTTPS; refuses bodies over `max_bytes` or transfers
// that take longer than `timeout_seconds` in total.
Download get(std::wstring_view url, const std::filesystem::path& destination,
    std::uint64_t max_bytes, std::uint32_t timeout_seconds, const wchar_t* agent, Watch* watch = nullptr);
// The body of the same GET, for an answer small enough to hold: nothing when it
// fails, and `result`, when given, says how.
std::optional<std::string> get_text(std::wstring_view url, std::uint64_t max_bytes,
    std::uint32_t timeout_seconds, const wchar_t* agent, Download* result = nullptr);
}
