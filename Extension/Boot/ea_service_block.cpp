#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "ea_service_block.h"

#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"

#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dingosdk {
namespace {

using GetAddrInfoAFn = INT (WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
using GetAddrInfoWFn = INT (WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
using GetAddrInfoExAFn = INT (WSAAPI*)(PCSTR, PCSTR, DWORD, LPGUID, const ADDRINFOEXA*, PADDRINFOEXA*, timeval*,
    LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
using GetAddrInfoExWFn = INT (WSAAPI*)(PCWSTR, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW*, PADDRINFOEXW*, timeval*,
    LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
using GetHostByNameFn = hostent* (WSAAPI*)(const char*);

std::atomic<GetAddrInfoAFn> original_get_addr_info_a{};
std::atomic<GetAddrInfoWFn> original_get_addr_info_w{};
std::atomic<GetAddrInfoExAFn> original_get_addr_info_ex_a{};
std::atomic<GetAddrInfoExWFn> original_get_addr_info_ex_w{};
std::atomic<GetHostByNameFn> original_get_host_by_name{};

// A refused name is logged once at info level; a service that cannot connect keeps retrying, so
// repeats are logged at debug level only. Never destroyed, because lookups can still arrive while
// the process exits.
struct RefusedNames {
    std::mutex mutex;
    std::set<std::wstring> names;
};
RefusedNames& refused_names() {
    static auto* value = new RefusedNames;
    return *value;
}

template<class Char>
bool refuse(const Char* name) noexcept {
    if (!name) return false;
    const std::basic_string_view<Char> host(name);
    if (!ea_service_host(host)) return false;
    try {
        std::wstring wide;
        wide.reserve(host.size());
        for (const auto ch : host) wide.push_back(static_cast<wchar_t>(static_cast<std::make_unsigned_t<Char>>(ch)));
        bool first{};
        {
            auto& refused = refused_names();
            const std::lock_guard lock(refused.mutex);
            first = refused.names.insert(wide).second;
        }
        if (first)
            logging::write(logging::Level::info, logging::Channel::runtime,
                L"Blocked the game's lookup of " + wide + L": ReSkate keeps it off EA's online services");
        else if (logging::enabled(logging::Level::debug))
            logging::write(logging::Level::debug, logging::Channel::runtime, L"Blocked the game's lookup of " + wide + L" again");
    } catch (...) {}
    return true;
}

// What the resolver itself answers for a name that does not exist.
template<class Result>
INT not_found(Result* result) noexcept {
    if (result) *result = nullptr;
    WSASetLastError(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
}

INT WSAAPI get_addr_info_a(PCSTR name, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    if (refuse(name)) return not_found(result);
    return original_get_addr_info_a.load()(name, service, hints, result);
}

INT WSAAPI get_addr_info_w(PCWSTR name, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result) {
    if (refuse(name)) return not_found(result);
    return original_get_addr_info_w.load()(name, service, hints, result);
}

// Failing before the lookup starts is a valid answer for an overlapped call too: the completion
// routine and event are only used once the function has returned WSA_IO_PENDING.
INT WSAAPI get_addr_info_ex_a(PCSTR name, PCSTR service, DWORD name_space, LPGUID provider, const ADDRINFOEXA* hints,
                              PADDRINFOEXA* result, timeval* timeout, LPOVERLAPPED overlapped,
                              LPLOOKUPSERVICE_COMPLETION_ROUTINE completion, LPHANDLE handle) {
    if (refuse(name)) {
        if (handle) *handle = nullptr;
        return not_found(result);
    }
    return original_get_addr_info_ex_a.load()(name, service, name_space, provider, hints, result, timeout, overlapped,
        completion, handle);
}

INT WSAAPI get_addr_info_ex_w(PCWSTR name, PCWSTR service, DWORD name_space, LPGUID provider, const ADDRINFOEXW* hints,
                              PADDRINFOEXW* result, timeval* timeout, LPOVERLAPPED overlapped,
                              LPLOOKUPSERVICE_COMPLETION_ROUTINE completion, LPHANDLE handle) {
    if (refuse(name)) {
        if (handle) *handle = nullptr;
        return not_found(result);
    }
    return original_get_addr_info_ex_w.load()(name, service, name_space, provider, hints, result, timeout, overlapped,
        completion, handle);
}

hostent* WSAAPI get_host_by_name(const char* name) {
    if (refuse(name)) {
        WSASetLastError(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    return original_get_host_by_name.load()(name);
}

// `required`: the entry point the game itself resolves names through; without it nothing is
// blocked, and that is an error. The others close the same door for anything else loaded into
// the process, and are left alone where they cannot be hooked: Wine's ws2_32 (Proton, on Linux
// and the Steam Deck) does not have all of them, and what it does not have nothing can call.
template<class Fn>
void hook(const char* name, void* replacement, std::atomic<Fn>& original, bool required) {
    const auto fail = [&](const std::string& why) {
        if (required) throw std::runtime_error(why);
        logging::log(logging::Level::info, logging::Channel::runtime, "EA online services block: {} left as it is ({}).", name, why);
    };
    const auto module = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto target = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    if (!target) return fail(std::string("Missing ") + name);
    void* relay{};
    const auto create = hook_prepare(target, replacement, &relay);
    if (create != HookOk || !relay) {
        if (create == HookOk) (void)hook_remove(target);
        return fail(std::string("Cannot hook ") + name + ": " + hook_status_string(create == HookOk ? HookUnsupportedFunction : create));
    }
    original.store(reinterpret_cast<Fn>(relay));
    const auto enable = hook_enable(target);
    if (enable != HookOk) {
        (void)hook_remove(target);
        return fail(std::string("Cannot enable ") + name + ": " + hook_status_string(enable));
    }
}

} // namespace

bool start_ea_service_block(std::string& error) noexcept {
    try {
        // Skate.exe resolves names through getaddrinfo alone; the other entry points close the
        // same door for anything else loaded into the process.
#pragma warning(push)
#pragma warning(disable: 4191)
        hook("getaddrinfo", reinterpret_cast<void*>(&get_addr_info_a), original_get_addr_info_a, true);
        hook("GetAddrInfoW", reinterpret_cast<void*>(&get_addr_info_w), original_get_addr_info_w, false);
        hook("GetAddrInfoExA", reinterpret_cast<void*>(&get_addr_info_ex_a), original_get_addr_info_ex_a, false);
        hook("GetAddrInfoExW", reinterpret_cast<void*>(&get_addr_info_ex_w), original_get_addr_info_ex_w, false);
        hook("gethostbyname", reinterpret_cast<void*>(&get_host_by_name), original_get_host_by_name, false);
#pragma warning(pop)
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace dingosdk
