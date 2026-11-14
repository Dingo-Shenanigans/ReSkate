#pragma once
#include <string_view>

namespace dingosdk::https {
// Whether a server's certificate (a PCCERT_CONTEXT, with the certificates the server sent beside
// it) is good for `host` and chains to one of the roots ReSkate carries itself (isrg_roots.h),
// checked without the Windows certificate store: name, dates, usage and every signature on the
// way. Some players' Windows refused api.reskate.dev's Let's Encrypt chain while their browsers,
// which carry their own roots, took it (WinHTTP error 12175, in the game and the launcher alike):
// a request Windows refuses for that is tried again and kept only when this says yes.
bool chains_to_builtin_root(const void* certificate, std::wstring_view host);
}
