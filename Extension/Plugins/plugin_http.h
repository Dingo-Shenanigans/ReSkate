#pragma once
#include "plugin_vm.h"
#include <vector>

// net.http in the game: each request on a thread of its own, with WinHTTP, HTTPS only. A host that
// resolves to a private, local or reserved address is refused, redirects are not followed,
// cookies and Windows sign-in are off, and an answer is at most 1 MB and 10 s away.
namespace dingosdk::plugins::http {
// False when too many requests are going on already (8 for all plugins).
bool start(HttpRequest request) noexcept;
// The answers that came in since the last call. Client thread.
std::vector<HttpResponse> take() noexcept;
}
