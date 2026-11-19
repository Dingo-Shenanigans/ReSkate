#pragma once
#include "plugin_vm.h"
#include <vector>

// net.http in the game: each request on a thread of its own, with WinHTTP, HTTPS only. A host that
// resolves to a private, local or reserved address is refused, cookies and Windows sign-in are off,
// and a redirect is followed only when the request asks, and only to a host the plugin's manifest
// lists. An answer is as large and as long in coming as the request allows (1 MB and 10 s unless it
// says), kept in memory or written to a file of the plugin's, where one that broke off can be gone on with.
namespace dingosdk::plugins::http {
// False when too many requests are going on already (16 for all plugins).
bool start(HttpRequest request) noexcept;
// How far a request going on has come; nothing once it is answered. Any thread.
std::optional<HttpProgress> progress(std::string_view plugin, std::uint64_t id) noexcept;
// Gives one up: it is answered with "it was given up" at its next read.
void cancel(std::string_view plugin, std::uint64_t id) noexcept;
// The answers that came in since the last call. Client thread.
std::vector<HttpResponse> take() noexcept;
}
